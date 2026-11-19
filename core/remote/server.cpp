#include "remote/server.h"

#include "net/lan.h"
#include "remote/qr.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostInfo>
#include <QHttpServerResponse>
#include <QHttpServerWebSocketUpgradeResponse>
#include <QJsonDocument>
#include <QNetworkInterface>
#include <QPointer>
#include <QUrl>
#include <QtConcurrent/QtConcurrentRun>

namespace resindeck {

namespace {

using HttpStatus = QHttpServerResponder::StatusCode;

const QStringList kOpenPaths{"/api/session", "/api/login"};

// One of this machine's addresses (only local addresses can be bound).
bool ownAddress(const QHostAddress &address) {
    static QList<QHostAddress> cache;
    static qint64 checked = 0;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - checked > 60000) {
        cache = QNetworkInterface::allAddresses();
        checked = now;
    }
    for (const QHostAddress &a : cache)
        if (a.isEqual(address, QHostAddress::TolerantConversion)) return true;
    return false;
}

bool trustedHost(const QByteArray &hostHeader) {
    const QUrl url("http://" + QString::fromLatin1(hostHeader));
    const QString name = url.host().toLower();
    if (!QHostAddress(name).isNull()) return true;
    return name == "localhost" || name == QHostInfo::localHostName().toLower() || name.endsWith(".local");
}

QString cookie(const QHttpHeaders &headers, const QString &name) {
    for (const QByteArray &line : headers.values(QHttpHeaders::WellKnownHeader::Cookie))
        for (const QByteArray &part : line.split(';')) {
            const QByteArray kv = part.trimmed();
            const qsizetype eq = kv.indexOf('=');
            if (eq > 0 && kv.left(eq) == name.toLatin1()) return QString::fromLatin1(kv.mid(eq + 1));
        }
    return {};
}

QByteArray mimeType(const QString &path) {
    static const QHash<QString, QByteArray> types{
        {"html", "text/html; charset=utf-8"}, {"js", "text/javascript; charset=utf-8"},
        {"css", "text/css; charset=utf-8"},   {"svg", "image/svg+xml"},
        {"png", "image/png"},                 {"ico", "image/x-icon"},
        {"json", "application/json"},         {"woff2", "font/woff2"},
        {"webmanifest", "application/manifest+json"}};
    return types.value(QFileInfo(path).suffix().toLower(), "application/octet-stream");
}

QByteArray pngBytes(const QImage &img) {
    QByteArray out;
    QBuffer buf(&out);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, "PNG");
    return out;
}

// Parts of a multipart/form-data body: {file name, contents}.
QList<std::pair<QString, QByteArray>> multipartFiles(const QByteArray &contentType, const QByteArray &body) {
    QList<std::pair<QString, QByteArray>> files;
    const qsizetype b = contentType.indexOf("boundary=");
    if (b < 0) return files;
    QByteArray boundary = contentType.mid(b + 9);
    if (boundary.startsWith('"')) boundary = boundary.mid(1, boundary.indexOf('"', 1) - 1);
    const QByteArray delimiter = "--" + boundary;
    qsizetype pos = body.indexOf(delimiter);
    while (pos >= 0) {
        pos += delimiter.size();
        if (body.mid(pos, 2) == "--") break;  // final delimiter
        const qsizetype headersEnd = body.indexOf("\r\n\r\n", pos);
        if (headersEnd < 0) break;
        const QByteArray partHeaders = body.mid(pos, headersEnd - pos);
        const qsizetype next = body.indexOf("\r\n" + delimiter, headersEnd + 4);
        if (next < 0) break;
        const QByteArray data = body.mid(headersEnd + 4, next - headersEnd - 4);
        const qsizetype fn = partHeaders.indexOf("filename=\"");
        if (fn >= 0) {
            const qsizetype start = fn + 10;
            const QString name = QString::fromUtf8(partHeaders.mid(start, partHeaders.indexOf('"', start) - start));
            files.append({name, data});
        }
        pos = next + 2;
    }
    return files;
}

}  // namespace

struct Server::Request {
    const QHttpServerRequest &http;
    QRegularExpressionMatch match;
    bool local;

    QString arg(const QString &name) const { return match.captured(name); }
    QString query(const QString &name) const { return http.query().queryItemValue(name, QUrl::FullyDecoded); }
    // The body as a JSON object; PrinterError("bad_request") when it is not one.
    QJsonObject json() const {
        const QJsonDocument doc = QJsonDocument::fromJson(http.body());
        if (!doc.isObject()) throw PrinterError("Invalid request.", "bad_request");
        return doc.object();
    }
    void localOnly() const {
        if (!local) throw HttpError(403, "only from the computer running ResinDeck");
    }
};

Server::Server(Deck &deck, QString webDir, QObject *parent) : QObject(parent), deck_(deck), webDir_(std::move(webDir)) {
    isLocal = [](const QHostAddress &address, const QHttpHeaders &) {
        return address.isLoopback() || ownAddress(address);
    };
    addRoutes();
    http_.setMissingHandler(this, [this](const QHttpServerRequest &request, QHttpServerResponder &responder) {
        handle(request, responder);
    });
    http_.addWebSocketUpgradeVerifier(this, [this](const QHttpServerRequest &request) {
        if (request.url().path() != "/ws") return QHttpServerWebSocketUpgradeResponse::passToNext();
        const bool local = isLocal(request.remoteAddress(), request.headers());
        if (const QString why = guard(request, local); !why.isEmpty())
            return QHttpServerWebSocketUpgradeResponse::deny(403, why.toUtf8());
        if (!authorized(request, local)) return QHttpServerWebSocketUpgradeResponse::deny(401, "pin_required");
        const auto device = local ? std::nullopt : deviceOf(request.headers());
        pendingUpgrades_.insert(QString("%1:%2").arg(request.remoteAddress().toString()).arg(request.remotePort()),
                                {local, device ? device->value("id").toString() : QString()});
        return QHttpServerWebSocketUpgradeResponse::accept();
    });
    connect(&http_, &QHttpServer::newWebSocketConnection, this, &Server::onWebSocket);

    connect(&deck_, &Deck::printersChanged, this, [this] { broadcast({{"type", "printers"}, {"printers", deck_.printerList()}}); });
    connect(&deck_, &Deck::devicesChanged, this, [this] { broadcast({{"type", "devices"}}, true); });
    connect(&deck_, &Deck::voxelProgress, this, [this](const QString &path, double pct) {
        broadcast({{"type", "voxels"}, {"path", path}, {"pct", pct}});
    });
    connect(&deck_, &Deck::deviceDropped, this, [this](const QString &id) {
        for (Client &c : clients_)
            if (!c.device.isEmpty() && (id.isEmpty() || c.device == id)) c.socket->close();
    });
    heartbeat_.setInterval(30000);
    connect(&heartbeat_, &QTimer::timeout, this, [this] {
        for (Client &c : clients_) c.socket->ping();
    });
    heartbeat_.start();
}

Server::~Server() {
    mdns_.stop();
    for (Client &c : clients_) {
        c.socket->disconnect(this);
        c.socket->abort();
    }
    clients_.clear();
}

bool Server::listen(quint16 port, bool lan) {
    lan_ = lan;
    auto *tcp = new QTcpServer(&http_);
    // on purpose: phones reach it over the LAN; guard() and the PIN protect the API
    if (!tcp->listen(lan ? QHostAddress::AnyIPv4 : QHostAddress::LocalHost, port) || !http_.bind(tcp)) {
        delete tcp;
        return false;
    }
    tcp_ = tcp;
    const QString ip = lan ? lan::localIp() : QString();
    if (!ip.isEmpty() && !mdns_.start(QHostAddress(ip)))
        std::fprintf(stderr, "mDNS unavailable (port 5353); use the IP address instead\n");
    return true;
}

quint16 Server::port() const { return tcp_ ? tcp_->serverPort() : 0; }

QStringList Server::remoteUrls() const {
    const QString ip = lan_ ? lan::localIp() : QString();
    QStringList urls;
    if (!ip.isEmpty() && mdns_.running()) urls << QString("http://%1:%2/").arg(mdns::kName).arg(port());
    if (!ip.isEmpty()) urls << QString("http://%1:%2/").arg(ip).arg(port());
    return urls;
}

QString Server::findWebDir() {
    const QString app = QCoreApplication::applicationDirPath();
    for (const QString &dir : {app + "/web", app + "/../share/resindeck/web", app + "/../web", QString(RESINDECK_SOURCE_DIR) + "/static"})
        if (QFileInfo::exists(dir + "/index.html")) return QDir(dir).canonicalPath();
    return QString(RESINDECK_SOURCE_DIR) + "/static";
}

// --- security ----------------------------------------------------------------------------

QString Server::guard(const QHttpServerRequest &request, bool local) const {
    const QByteArray host = request.value("Host");
    if (local && !trustedHost(host)) return "unexpected Host header";
    const QByteArray origin = request.value("Origin");
    if (!origin.isEmpty()) {
        const QUrl o(QString::fromLatin1(origin));
        const QString netloc = o.port() < 0 ? o.host() : QString("%1:%2").arg(o.host()).arg(o.port());
        if (netloc.compare(QString::fromLatin1(host), Qt::CaseInsensitive) != 0) return "cross-origin request";
    }
    return {};
}

std::optional<QJsonObject> Server::deviceOf(const QHttpHeaders &headers) const {
    return deck_.device(cookie(headers, kSessionCookie));
}

bool Server::authorized(const QHttpServerRequest &request, bool local) const {
    return local || deviceOf(request.headers()).has_value();
}

QByteArray Server::contentSecurityPolicy() const {
    if (csp_.isEmpty()) {
        // the import map is the only inline script: allow exactly that one by its hash
        QFile f(webDir_ + "/index.html");
        QByteArray script = "'self'";
        if (f.open(QIODevice::ReadOnly)) {
            const QString html = QString::fromUtf8(f.readAll());
            static const QRegularExpression re(R"re(<script type="importmap">(.*?)</script>)re",
                                               QRegularExpression::DotMatchesEverythingOption);
            const auto m = re.match(html);
            if (m.hasMatch())
                script += " 'sha256-" + QCryptographicHash::hash(m.captured(1).toUtf8(), QCryptographicHash::Sha256).toBase64() + "'";
        }
        csp_ = "default-src 'self'; script-src " + script +
               "; style-src 'self' 'unsafe-inline'; "  // progress bars set style="width:…"
               "img-src 'self' data: blob:; connect-src 'self'; object-src 'none'; "
               "base-uri 'none'; form-action 'self'; frame-ancestors 'none'";
    }
    return csp_;
}

// --- responses ---------------------------------------------------------------------------

void Server::send(Responder r, QHttpServerResponse response) const {
    QHttpHeaders h = response.headers();
    auto setDefault = [&](const QByteArray &name, const QByteArray &value) {
        if (!h.contains(name)) h.append(name, value);
    };
    setDefault("X-Content-Type-Options", "nosniff");
    setDefault("X-Frame-Options", "DENY");
    setDefault("Referrer-Policy", "no-referrer");
    setDefault("Cross-Origin-Resource-Policy", "same-origin");
    setDefault("Content-Security-Policy", contentSecurityPolicy());
    response.setHeaders(std::move(h));
    r->sendResponse(response);
}

void Server::sendJson(Responder r, const QJsonValue &json, int status) const {
    const QByteArray body = json.isArray() ? QJsonDocument(json.toArray()).toJson(QJsonDocument::Compact)
                                           : QJsonDocument(json.toObject()).toJson(QJsonDocument::Compact);
    send(std::move(r), QHttpServerResponse("application/json", body, static_cast<HttpStatus>(status)));
}

void Server::sendError(Responder r, std::exception_ptr error) const {
    try {
        std::rethrow_exception(error);
    } catch (const PrinterError &e) {
        sendJson(std::move(r), QJsonObject{{"error", e.message()}, {"code", e.code()}, {"params", QJsonObject::fromVariantMap(e.params())}}, 400);
    } catch (const NetworkError &e) {
        const QString detail = QString::fromUtf8(e.what());
        sendJson(std::move(r), QJsonObject{{"error", "Communication failure: " + detail}, {"code", "comm_failure"},
                                           {"params", QJsonObject{{"detail", detail}}}},
                 502);
    } catch (const HttpError &e) {
        send(std::move(r), QHttpServerResponse("text/plain; charset=utf-8", QByteArray(e.what()), static_cast<HttpStatus>(e.status())));
    } catch (const std::exception &e) {
        send(std::move(r), QHttpServerResponse("text/plain; charset=utf-8", QByteArray(e.what()), HttpStatus::InternalServerError));
    }
}

void Server::sendFuture(Responder r, QFuture<QJsonObject> future) {
    future.then(this, [this, r](QFuture<QJsonObject> f) {
        try {
            sendJson(r, f.result());
        } catch (...) {
            sendError(r, std::current_exception());
        }
    });
}

void Server::serveFile(Responder r, const QString &path, bool cache) const {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        send(std::move(r), QHttpServerResponse(HttpStatus::NotFound));
        return;
    }
    QHttpServerResponse res(mimeType(path), f.readAll());
    if (!cache) {
        QHttpHeaders h = res.headers();
        h.append("Cache-Control", "no-cache");
        res.setHeaders(std::move(h));
    }
    send(std::move(r), std::move(res));
}

// --- routing -----------------------------------------------------------------------------

void Server::route(QHttpServerRequest::Method method, const QString &pattern, Handler handler) {
    routes_.push_back({method, QRegularExpression("^" + pattern + "$"), std::move(handler)});
}

void Server::handle(const QHttpServerRequest &request, QHttpServerResponder &responder) {
    auto r = std::make_shared<QHttpServerResponder>(std::move(responder));
    const QString path = request.url().path();
    const bool local = isLocal(request.remoteAddress(), request.headers());
    if (const QString why = guard(request, local); !why.isEmpty()) {
        send(r, QHttpServerResponse("text/plain; charset=utf-8", why.toUtf8(), HttpStatus::Forbidden));
        return;
    }
    const bool isProtected = path.startsWith("/api/") || path == "/ws";
    if (isProtected && !kOpenPaths.contains(path) && !authorized(request, local)) {
        sendJson(r, QJsonObject{{"error", "Enter the PIN shown on the computer."}, {"code", "pin_required"}, {"params", QJsonObject{}}}, 401);
        return;
    }
    bool pathMatched = false;
    for (const Route &rt : routes_) {
        const auto m = rt.pattern.match(path);
        if (!m.hasMatch()) continue;
        pathMatched = true;
        if (rt.method != request.method()) continue;
        try {
            rt.handler(Request{request, m, local}, r);
        } catch (...) {
            sendError(r, std::current_exception());
        }
        return;
    }
    send(r, QHttpServerResponse(pathMatched ? HttpStatus::MethodNotAllowed : HttpStatus::NotFound));
}

void Server::addRoutes() {
    using M = QHttpServerRequest::Method;

    route(M::Get, "/", [this](const Request &, Responder r) { serveFile(r, webDir_ + "/index.html", false); });

    route(M::Get, "/static/(?<file>.+)", [this](const Request &q, Responder r) {
        const QString root = QDir(webDir_).canonicalPath();
        const QString file = QFileInfo(root + "/" + q.arg("file")).canonicalFilePath();
        if (file.isEmpty() || !file.startsWith(root + "/")) throw HttpError(404, "not found");
        serveFile(r, file, false);
    });

    // --- session ---

    route(M::Get, "/api/session", [this](const Request &q, Responder r) {
        sendJson(r, QJsonObject{{"local", q.local}, {"authorized", authorized(q.http, q.local)}});
    });

    route(M::Post, "/api/login", [this](const Request &q, Responder r) {
        const QString token = deck_.login(q.json().value("pin").toVariant().toString(), q.http.remoteAddress().toString(),
                                          QString::fromUtf8(q.http.value("User-Agent")));
        QHttpServerResponse res("application/json", R"({"ok":true})");
        QHttpHeaders h = res.headers();
        h.append(QHttpHeaders::WellKnownHeader::SetCookie,
                 QString("%1=%2; Max-Age=%3; HttpOnly; SameSite=Strict; Path=/").arg(kSessionCookie, token).arg(365 * 86400).toLatin1());
        res.setHeaders(std::move(h));
        send(r, std::move(res));
    });

    // --- phones ---

    route(M::Get, "/api/remote", [this](const Request &q, Responder r) {
        q.localOnly();
        const QStringList urls = remoteUrls();
        sendJson(r, QJsonObject{{"enabled", !urls.isEmpty()}, {"urls", QJsonArray::fromStringList(urls)}, {"pin", deck_.pin()},
                                {"devices", deck_.deviceList(onlineDevices())}});
    });

    route(M::Delete, "/api/remote/devices/(?<id>[^/]+)", [this](const Request &q, Responder r) {
        q.localOnly();
        deck_.removeDevice(q.arg("id"));
        sendJson(r, QJsonObject{{"ok", true}});
    });

    route(M::Get, "/api/remote/qr.svg", [this](const Request &q, Responder r) {
        q.localOnly();
        const QStringList urls = remoteUrls();
        if (urls.isEmpty()) throw HttpError(404, "not on a network");
        // resindeck.local, or the IP when mDNS is not available; the PIN is typed, never in the code
        QHttpServerResponse res("image/svg+xml", qr::svg(urls.first()));
        QHttpHeaders h = res.headers();
        h.append("Cache-Control", "no-store");
        res.setHeaders(std::move(h));
        send(r, std::move(res));
    });

    route(M::Post, "/api/remote/pin", [this](const Request &q, Responder r) {
        q.localOnly();
        sendJson(r, QJsonObject{{"pin", deck_.renewPin()}});
    });

    route(M::Post, "/api/quit", [this](const Request &q, Responder r) {
        q.localOnly();
        sendJson(r, QJsonObject{{"ok", true}});
        QTimer::singleShot(200, &deck_, &Deck::requestQuit);
    });

    // --- printers ---

    route(M::Get, "/api/drivers", [this](const Request &, Responder r) {
        QJsonArray list;
        for (const DriverType &t : driverTypes()) list.append(t.info.toJson());
        sendJson(r, list);
    });

    route(M::Get, "/api/printers", [this](const Request &, Responder r) { sendJson(r, deck_.printerList()); });

    route(M::Post, "/api/printers", [this](const Request &q, Responder r) {
        q.localOnly();
        sendJson(r, QJsonObject{{"id", deck_.addPrinter(q.json())}});
    });

    route(M::Post, "/api/printers/scan", [this](const Request &q, Responder r) {
        q.localOnly();
        deck_.scan().then(this, [this, r](const QJsonArray &found) { sendJson(r, found); });
    });

    route(M::Put, "/api/printers/(?<id>[^/]+)", [this](const Request &q, Responder r) {
        q.localOnly();
        deck_.editPrinter(q.arg("id"), q.json());
        sendJson(r, QJsonObject{{"ok", true}});
    });

    route(M::Delete, "/api/printers/(?<id>[^/]+)", [this](const Request &q, Responder r) {
        q.localOnly();
        deck_.removePrinter(q.arg("id"));
        sendJson(r, QJsonObject{{"ok", true}});
    });

    route(M::Post, "/api/printers/(?<id>[^/]+)/send", [this](const Request &q, Responder r) {
        Printer &p = deck_.printerOr404(q.arg("id"));
        const QJsonObject body = q.json();
        const QString path = deck_.resolve(body.value("path").toString());
        sendFuture(r, p.send(path, body.value("print").toBool()));
    });

    route(M::Post, "/api/printers/(?<id>[^/]+)/(?<action>print|pause|resume|stop)", [this](const Request &q, Responder r) {
        Printer &p = deck_.printerOr404(q.arg("id"));
        const QString action = q.arg("action");
        if (action == "print") {
            const QString filename = q.json().value("filename").toString();
            if (filename.isEmpty()) throw PrinterError("Invalid request.", "bad_request");
            sendFuture(r, p.command("start", filename));
        } else {
            sendFuture(r, p.command(action));
        }
    });

    // --- library ---

    route(M::Get, "/api/library", [this](const Request &, Responder r) { sendJson(r, deck_.library()); });

    route(M::Put, "/api/library/folders", [this](const Request &q, Responder r) {
        q.localOnly();  // a paired phone must not point the library at other folders
        sendJson(r, QJsonObject{{"folders", QJsonArray::fromStringList(deck_.setFolders(q.json().value("folders")))}});
    });

    route(M::Post, "/api/library/upload", [this](const Request &q, Responder r) {
        if (deck_.folders().isEmpty()) throw PrinterError("Set up a library folder first.", "no_folder");
        QStringList saved;
        for (const auto &[name, data] : multipartFiles(q.http.value("Content-Type"), q.http.body()))
            saved << deck_.saveUpload(name, data);
        sendJson(r, QJsonObject{{"saved", QJsonArray::fromStringList(saved)}});
    });

    route(M::Get, "/api/file/info", [this](const Request &q, Responder r) {
        sendJson(r, deck_.fileInfo(deck_.resolve(q.query("path"))));
    });

    route(M::Get, "/api/file/preview", [this](const Request &q, Responder r) {
        const auto f = deck_.file(deck_.resolve(q.query("path")));
        if (!f->previewCount()) throw HttpError(404, "no preview");
        const int index = q.query("size") == "small" ? 0 : -1;
        QtConcurrent::run([f, index] { return pngBytes(f->preview(index)); }).then(this, [this, r](const QByteArray &png) {
            QHttpServerResponse res("image/png", png);
            QHttpHeaders h = res.headers();
            h.append("Cache-Control", "max-age=3600");
            res.setHeaders(std::move(h));
            send(r, std::move(res));
        });
    });

    route(M::Get, "/api/file/layer", [this](const Request &q, Responder r) {
        const auto f = deck_.file(deck_.resolve(q.query("path")));
        bool ok = false;
        const int n = q.query("n").isEmpty() ? 0 : q.query("n").toInt(&ok);
        if ((!ok && !q.query("n").isEmpty()) || n < 0 || n >= f->layerCount()) throw HttpError(400, "invalid layer");
        QtConcurrent::run([f, n] { return pngBytes(f->layer(n)); }).then(this, [this, r](const QByteArray &png) {
            QHttpServerResponse res("image/png", png);
            QHttpHeaders h = res.headers();
            h.append("Cache-Control", "max-age=3600");
            res.setHeaders(std::move(h));
            send(r, std::move(res));
        });
    });

    route(M::Get, "/api/file/voxels", [this](const Request &q, Responder r) {
        deck_.voxels(deck_.resolve(q.query("path")), [this, r](const QByteArray &payload, std::exception_ptr error) {
            if (error)
                sendError(r, error);
            else
                send(r, QHttpServerResponse("application/octet-stream", payload));
        });
    });
}

// --- WebSocket ---------------------------------------------------------------------------

void Server::onWebSocket() {
    while (http_.hasPendingWebSocketConnections()) {
        std::unique_ptr<QWebSocket> ws = http_.nextPendingWebSocketConnection();
        const QString key = QString("%1:%2").arg(ws->peerAddress().toString()).arg(ws->peerPort());
        const auto [local, device] = pendingUpgrades_.take(key);
        QWebSocket *raw = ws.get();
        connect(raw, &QWebSocket::disconnected, this, [this, raw] {
            const auto it = std::find_if(clients_.begin(), clients_.end(), [raw](const Client &c) { return c.socket.get() == raw; });
            if (it == clients_.end()) return;
            const bool wasDevice = !it->device.isEmpty();
            it->socket.release()->deleteLater();
            clients_.erase(it);
            if (wasDevice) emit deck_.devicesChanged();
        });
        raw->sendTextMessage(QJsonDocument(QJsonObject{{"type", "printers"}, {"printers", deck_.printerList()}}).toJson(QJsonDocument::Compact));
        clients_.push_back({std::move(ws), local, device});
        if (!device.isEmpty()) emit deck_.devicesChanged();
    }
}

void Server::broadcast(const QJsonObject &message, bool localOnly) {
    const QString data = QString::fromUtf8(QJsonDocument(message).toJson(QJsonDocument::Compact));
    for (Client &c : clients_)
        if (!localOnly || c.local) c.socket->sendTextMessage(data);
}

QSet<QString> Server::onlineDevices() const {
    QSet<QString> online;
    for (const Client &c : clients_)
        if (!c.device.isEmpty()) online << c.device;
    return online;
}

}  // namespace resindeck
