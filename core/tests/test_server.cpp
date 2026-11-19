// The web server: security, pairing, the API and the WebSocket. Ported from tests/test_server.py.
// Requests with an "X-Test-Remote" header act as another device on the network.
#include "fakeprinter.h"
#include "formats/cxdlp.h"
#include "net/mdns.h"
#include "remote/qr.h"
#include "remote/server.h"

#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTest>
#include <QWebSocket>
#include <QtEndian>

#include <cstring>

using namespace resindeck;

namespace {

class FakeNetwork : public lan::Network {
public:
    QString resolve(const QString &host) override { return lan::hostPart(host); }
    QHash<QString, QString> neighbors() override { return {}; }
    QStringList hosts() override { return {"127.0.0.1"}; }
};

struct Reply {
    int status = 0;
    QByteArray body;
    QHash<QByteArray, QByteArray> headers;
    QJsonObject json() const { return QJsonDocument::fromJson(body).object(); }
    QJsonArray array() const { return QJsonDocument::fromJson(body).array(); }
};

const QHash<QByteArray, QByteArray> kPhone{{"X-Test-Remote", "1"}, {"User-Agent", "Mozilla/5.0 (Linux; Android 14; Pixel) Mobile Chrome/131"}};

}  // namespace

class TestServer : public QObject {
    Q_OBJECT

    QTemporaryDir home;
    QString lib;
    QString sliced;
    std::unique_ptr<Deck> deck;
    std::unique_ptr<Server> server;
    QNetworkAccessManager net;

    QString url(const QString &path) const { return QString("http://127.0.0.1:%1%2").arg(server->port()).arg(path); }

    Reply call(const QByteArray &method, const QString &path, const QByteArray &body = {},
               const QHash<QByteArray, QByteArray> &headers = {}) {
        QNetworkRequest req{QUrl(url(path))};
        req.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
        req.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
        if (!body.isEmpty() && !headers.contains("Content-Type")) req.setRawHeader("Content-Type", "application/json");
        for (auto it = headers.begin(); it != headers.end(); ++it) req.setRawHeader(it.key(), it.value());
        QNetworkReply *r = net.sendCustomRequest(req, method, body);
        (void)QTest::qWaitFor([r] { return r->isFinished(); }, 30000);
        Reply out;
        out.status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        out.body = r->readAll();
        for (const auto &[k, v] : r->rawHeaderPairs()) out.headers.insert(k.toLower(), v);
        r->deleteLater();
        return out;
    }
    Reply get(const QString &path, const QHash<QByteArray, QByteArray> &h = {}) { return call("GET", path, {}, h); }
    Reply post(const QString &path, const QJsonObject &body = {}, const QHash<QByteArray, QByteArray> &h = {}) {
        return call("POST", path, QJsonDocument(body).toJson(QJsonDocument::Compact), h);
    }

    // Pairs a phone with the PIN; returns its headers, with the session cookie.
    QHash<QByteArray, QByteArray> pair() {
        const Reply r = post("/api/login", {{"pin", deck->pin()}}, kPhone);
        QHash<QByteArray, QByteArray> h = kPhone;
        h.insert("Cookie", r.headers.value("set-cookie").split(';').first());
        return h;
    }

    QString addPrinter(const FakePrinter &fake, const QString &password = "secret1") {
        const QString id = post("/api/printers", {{"name", "R6"}, {"host", fake.host()}, {"password", password}}).json().value("id").toString();
        Printer *p = deck->printer(id);
        if (!QTest::qWaitFor([p] { return p->online() && (p->status() || !p->error().isEmpty()); }, 5000)) return {};
        return id;
    }

private slots:
    void init() {
        QVERIFY(home.isValid());
        QDir(home.path()).removeRecursively();
        QDir().mkpath(home.path());
        qputenv("HOME", home.path().toLocal8Bit());
        qputenv("USERPROFILE", home.path().toLocal8Bit());
        lib = home.filePath("lib");
        QDir().mkpath(lib);
        sliced = lib + "/part.cxdlpv4";
        cxdlp::Writer w;
        for (int n = 0; n < 12; ++n) {
            QImage img(60, 40, QImage::Format_Grayscale8);
            img.fill(0);
            for (int y = 10; y < 30; ++y) std::memset(img.scanLine(y) + 20, 255, 20);
            w.layers.push_back(img);
        }
        w.write(sliced);
        QFile settings(home.filePath("settings.json"));
        QVERIFY(settings.open(QIODevice::WriteOnly));
        settings.write(QJsonDocument(QJsonObject{{"printers", QJsonArray{}}, {"folders", QJsonArray{lib}}, {"pin", "123456"}}).toJson());
        settings.close();
        deck = std::make_unique<Deck>(home.filePath("settings.json"), std::make_shared<FakeNetwork>(),
                                      PrinterTiming{50, 50, 60000, {}}, home.filePath("cache"));
        server = std::make_unique<Server>(*deck, QString(RESINDECK_SOURCE_DIR) + "/static");
        server->isLocal = [](const QHostAddress &, const QHttpHeaders &h) { return !h.contains("X-Test-Remote"); };
        QVERIFY(server->listen(0, false));
    }

    void cleanup() {
        server.reset();
        deck.reset();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);  // sockets closed with deleteLater
    }

    // --- pages and headers ---------------------------------------------------------------

    void indexHasSecurityHeaders() {
        const Reply r = get("/");
        QCOMPARE(r.status, 200);
        QVERIFY(r.body.contains("<html"));
        QCOMPARE(r.headers.value("x-frame-options"), QByteArray("DENY"));
        QCOMPARE(r.headers.value("x-content-type-options"), QByteArray("nosniff"));
        QCOMPARE(r.headers.value("referrer-policy"), QByteArray("no-referrer"));
        QVERIFY(r.headers.value("content-security-policy").contains("frame-ancestors 'none'"));
    }

    void cspHashMatchesTheImportMap() {
        const Reply r = get("/");
        const QRegularExpression re(R"re(<script type="importmap">(.*?)</script>)re", QRegularExpression::DotMatchesEverythingOption);
        const auto m = re.match(QString::fromUtf8(r.body));
        QVERIFY(m.hasMatch());
        const QByteArray hash = QCryptographicHash::hash(m.captured(1).toUtf8(), QCryptographicHash::Sha256).toBase64();
        QVERIFY(r.headers.value("content-security-policy").contains("'sha256-" + hash + "'"));
    }

    void staticFilesAndTraversal() {
        const Reply js = get("/static/app.js");
        QCOMPARE(js.status, 200);
        QVERIFY(js.headers.value("content-type").startsWith("text/javascript"));
        QVERIFY(js.headers.contains("content-security-policy"));
        QCOMPARE(get("/static/../server.py").status, 404);
        QCOMPARE(get("/static/%2e%2e/server.py").status, 404);
        QCOMPARE(get("/static/nope.js").status, 404);
        QCOMPARE(get("/nothing").status, 404);
        QCOMPARE(call("DELETE", "/api/printers").status, 405);
    }

    // --- pairing -------------------------------------------------------------------------

    void localSession() {
        QCOMPARE(get("/api/session").json(), (QJsonObject{{"local", true}, {"authorized", true}}));
    }

    void remoteNeedsThePin() {
        QCOMPARE(get("/api/session", kPhone).json(), (QJsonObject{{"local", false}, {"authorized", false}}));
        const Reply r = get("/api/printers", kPhone);
        QCOMPARE(r.status, 401);
        QCOMPARE(r.json().value("code").toString(), QString("pin_required"));
        QCOMPARE(get("/api/library", kPhone).status, 401);
    }

    void pairing() {
        const Reply r = post("/api/login", {{"pin", "123456"}}, kPhone);
        QCOMPARE(r.status, 200);
        const QByteArray cookie = r.headers.value("set-cookie");
        QVERIFY(cookie.startsWith("resindeck_session="));
        QVERIFY(cookie.contains("HttpOnly") && cookie.contains("SameSite=Strict"));
        auto phone = kPhone;
        phone.insert("Cookie", cookie.split(';').first());
        QCOMPARE(get("/api/session", phone).json().value("authorized").toBool(), true);
        QCOMPARE(get("/api/printers", phone).status, 200);
        // the settings keep a hash of the cookie, never the cookie itself
        QFile f(home.filePath("settings.json"));
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QByteArray saved = f.readAll();
        QVERIFY(!saved.contains(cookie.split(';').first().split('=').last()));
        QVERIFY(saved.contains("Android phone · Chrome"));
        QVERIFY(f.permissions() & QFileDevice::ReadOwner);
        QVERIFY(!(f.permissions() & QFileDevice::ReadOther));
    }

    void wrongPinAndRateLimit() {
        for (int i = 0; i < 5; ++i) QCOMPARE(post("/api/login", {{"pin", "000000"}}, kPhone).json().value("code").toString(), QString("pin_wrong"));
        QCOMPARE(post("/api/login", {{"pin", "123456"}}, kPhone).json().value("code").toString(), QString("pin_wait"));
    }

    void loginRejectsBadBodies() {
        QCOMPARE(call("POST", "/api/login", "not json", kPhone).json().value("code").toString(), QString("bad_request"));
        QCOMPARE(call("POST", "/api/login", "[1]", kPhone).json().value("code").toString(), QString("bad_request"));
        QCOMPARE(post("/api/login", {}, kPhone).json().value("code").toString(), QString("pin_wrong"));
    }

    void pairedDevicesCannotManageTheComputer() {
        const auto phone = pair();
        QCOMPARE(post("/api/printers", {{"host", "1.2.3.4"}}, phone).status, 403);
        QCOMPARE(call("PUT", "/api/library/folders", R"({"folders":["/"]})", phone).status, 403);
        QCOMPARE(post("/api/printers/scan", {}, phone).status, 403);
        QCOMPARE(get("/api/remote", phone).status, 403);
        QCOMPARE(get("/api/remote/qr.svg", phone).status, 403);
        QCOMPARE(post("/api/remote/pin", {}, phone).status, 403);
        QCOMPARE(post("/api/quit", {}, phone).status, 403);
        QCOMPARE(get("/api/library", phone).status, 200);
    }

    void disconnectOneDevice() {
        const auto phone = pair();
        const QJsonArray devices = get("/api/remote").json().value("devices").toArray();
        QCOMPARE(devices.size(), 1);
        QCOMPARE(devices[0].toObject().value("name").toString(), QString("Android phone · Chrome"));
        QCOMPARE(call("DELETE", "/api/remote/devices/" + devices[0].toObject().value("id").toString()).status, 200);
        QCOMPARE(get("/api/printers", phone).status, 401);
        QVERIFY(get("/api/remote").json().value("devices").toArray().isEmpty());
    }

    void newPinSignsEveryoneOut() {
        const auto phone = pair();
        const QString pin = post("/api/remote/pin").json().value("pin").toString();
        QCOMPARE(pin.size(), 6);
        QCOMPARE(pin, deck->pin());
        QCOMPARE(get("/api/printers", phone).status, 401);
    }

    void remoteDisabledWithoutNetwork() {
        // listening on 127.0.0.1 only: no address for phones, no QR code
        const QJsonObject r = get("/api/remote").json();
        QCOMPARE(r.value("enabled").toBool(), false);
        QCOMPARE(r.value("pin").toString(), QString("123456"));
        QCOMPARE(get("/api/remote/qr.svg").status, 404);
    }

    void qrCode() {
        const QByteArray svg = qr::svg("http://resindeck.local:8765/");
        QVERIFY(svg.startsWith("<svg xmlns=\"http://www.w3.org/2000/svg\""));
        const QImage img = qr::image("http://resindeck.local:8765/", 4, 2);
        QVERIFY(img.width() > 100 && img.width() == img.height());
        QCOMPARE(img.pixelColor(0, 0), QColor(Qt::white));  // the white border
        QCOMPARE(img.pixelColor(8, 8), QColor(Qt::black));  // the top-left finder pattern
    }

    // --- request checks ------------------------------------------------------------------

    void dnsRebindingIsBlocked() {
        QCOMPARE(get("/api/printers", {{"Host", "evil.example:8765"}}).status, 403);
        QCOMPARE(get("/api/printers", {{"Host", "resindeck.local:8765"}}).status, 200);
        QCOMPARE(get("/api/printers", {{"Host", "localhost:8765"}}).status, 200);
    }

    void crossOriginRequestsAreBlocked() {
        QCOMPARE(get("/api/printers", {{"Origin", "http://evil.example"}}).status, 403);
        QCOMPARE(post("/api/printers", {{"host", "1.2.3.4"}}, {{"Origin", "http://evil.example"}}).status, 403);
        QCOMPARE(get("/api/printers", {{"Origin", url("").toLatin1()}}).status, 200);
    }

    void crossSiteWebSocketIsBlocked() {
        QWebSocket ws("http://evil.example");
        QSignalSpy errors(&ws, &QWebSocket::errorOccurred);
        ws.open(QUrl(url("/ws").replace("http", "ws")));
        QVERIFY(QTest::qWaitFor([&] { return errors.count() > 0 || ws.state() == QAbstractSocket::ConnectedState; }, 5000));
        QVERIFY(ws.state() != QAbstractSocket::ConnectedState);
    }

    void remoteWebSocketNeedsThePin() {
        QNetworkRequest req(QUrl(url("/ws").replace("http", "ws")));
        req.setRawHeader("X-Test-Remote", "1");
        QWebSocket ws;
        QSignalSpy errors(&ws, &QWebSocket::errorOccurred);
        ws.open(req);
        QVERIFY(QTest::qWaitFor([&] { return errors.count() > 0 || ws.state() == QAbstractSocket::ConnectedState; }, 5000));
        QVERIFY(ws.state() != QAbstractSocket::ConnectedState);
    }

    // --- library -------------------------------------------------------------------------

    void libraryListsSlicedFiles() {
        QFile notes(lib + "/notes.txt");
        QVERIFY(notes.open(QIODevice::WriteOnly));
        notes.close();
        const QJsonObject r = get("/api/library").json();
        QCOMPARE(r.value("folders").toArray().first().toString(), QFileInfo(lib).canonicalFilePath());
        const QJsonArray files = r.value("files").toArray();
        QCOMPARE(files.size(), 1);
        QCOMPARE(files[0].toObject().value("name").toString(), QString("part.cxdlpv4"));
    }

    void setFolders() {
        const QString other = home.filePath("other");
        QDir().mkpath(other);
        Reply r = call("PUT", "/api/library/folders", QJsonDocument(QJsonObject{{"folders", QJsonArray{other, other}}}).toJson());
        QCOMPARE(r.json().value("folders").toArray().size(), 1);
        r = call("PUT", "/api/library/folders", R"({"folders":["/nonexistent/x"]})");
        QCOMPARE(r.json().value("code").toString(), QString("folder_missing"));
        QCOMPARE(call("PUT", "/api/library/folders", R"({"folders":"x"})").json().value("code").toString(), QString("bad_request"));
    }

    void filesOutsideTheLibraryAreRefused() {
        QCOMPARE(get("/api/file/info?path=/etc/passwd").status, 403);
        QCOMPARE(get("/api/file/info?path=" + QUrl::toPercentEncoding(lib + "/../settings.json")).status, 403);
        QCOMPARE(get("/api/file/info").status, 400);
        QCOMPARE(get("/api/file/info?path=" + QUrl::toPercentEncoding(lib + "/nope.cxdlpv4")).status, 404);
#ifndef Q_OS_WIN
        QFile::link(home.filePath("settings.json"), lib + "/link.cxdlpv4");  // a link inside pointing outside
        QCOMPARE(get("/api/file/info?path=" + QUrl::toPercentEncoding(lib + "/link.cxdlpv4")).status, 403);
#endif
    }

    void fileEndpoints() {
        const QString q = "?path=" + QUrl::toPercentEncoding(sliced);
        const QJsonObject info = get("/api/file/info" + q).json();
        QCOMPARE(info.value("layerCount").toInt(), 12);
        QCOMPARE(info.value("layers").toArray().size(), 12);
        QCOMPARE(info.value("hasVoxels").toBool(), false);
        for (const QString &extra : {QString(), QString("&size=small")}) {
            const Reply r = get("/api/file/preview" + q + extra);
            QCOMPARE(r.status, 200);
            QCOMPARE(r.headers.value("content-type"), QByteArray("image/png"));
            QCOMPARE(QImage::fromData(r.body).size(), QSize(16, 12));
        }
        const Reply layer = get("/api/file/layer" + q + "&n=5");
        QCOMPARE(QImage::fromData(layer.body).size(), QSize(60, 40));
        QCOMPARE(get("/api/file/layer" + q + "&n=12").status, 400);
        QCOMPARE(get("/api/file/layer" + q + "&n=x").status, 400);
        const Reply vox = get("/api/file/voxels" + q);
        QCOMPARE(vox.status, 200);
        const quint32 metaSize = qFromLittleEndian<quint32>(vox.body.constData());
        const QJsonObject meta = QJsonDocument::fromJson(vox.body.mid(4, metaSize)).object();
        QCOMPARE(meta.value("layersPerVoxel").toInt(), 12);  // 6 px of 0.1 mm per voxel, 0.05 mm layers
        QVERIFY(vox.body.size() > 4 + metaSize);
        QCOMPARE(get("/api/file/info" + q).json().value("hasVoxels").toBool(), true);
        QCOMPARE(get("/api/file/voxels" + q).body, vox.body);  // from the cache
    }

    void bothRequestsForTheSameVoxelsGetAnswered() {
        const QString q = "/api/file/voxels?path=" + QUrl::toPercentEncoding(sliced);
        QNetworkRequest a{QUrl(url(q))}, b{QUrl(url(q))};
        QNetworkReply *ra = net.get(a), *rb = net.get(b);
        QVERIFY(QTest::qWaitFor([&] { return ra->isFinished() && rb->isFinished(); }, 10000));
        QCOMPARE(ra->readAll(), rb->readAll());
        ra->deleteLater();
        rb->deleteLater();
    }

    void upload() {
        QFile f(sliced);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QByteArray data = f.readAll();
        auto multipart = [](const QString &name, const QByteArray &content) {
            return "--XyZ\r\nContent-Disposition: form-data; name=\"file\"; filename=\"" + name.toUtf8() +
                   "\"\r\nContent-Type: application/octet-stream\r\n\r\n" + content + "\r\n--XyZ--\r\n";
        };
        const QHash<QByteArray, QByteArray> type{{"Content-Type", "multipart/form-data; boundary=XyZ"}};
        Reply r = call("POST", "/api/library/upload", multipart("new part.cxdlpv4", data), type);
        QCOMPARE(r.status, 200);
        QCOMPARE(QFile(lib + "/new part.cxdlpv4").size(), data.size());
        r = call("POST", "/api/library/upload", multipart("../escape.cxdlpv4", data), type);
        QVERIFY(QFile::exists(lib + "/escape.cxdlpv4") && !QFile::exists(home.filePath("escape.cxdlpv4")));
        r = call("POST", "/api/library/upload", multipart("model.stl", "solid"), type);
        QCOMPARE(r.json().value("code").toString(), QString("only_cxdlp"));
        call("PUT", "/api/library/folders", R"({"folders":[]})");
        r = call("POST", "/api/library/upload", multipart("x.cxdlpv4", data), type);
        QCOMPARE(r.json().value("code").toString(), QString("no_folder"));
    }

    // --- printers ------------------------------------------------------------------------

    void drivers() {
        const QJsonArray d = get("/api/drivers").array();
        QCOMPARE(d.first().toObject().value("id").toString(), QString("halot"));
        QCOMPARE(d.first().toObject().value("passwordMax").toInt(), 8);
    }

    void printerCrudAndStatus() {
        FakePrinter fake;
        const QString id = addPrinter(fake);
        QVERIFY(!id.isEmpty());
        QJsonObject p = get("/api/printers").array().first().toObject();
        QCOMPARE(p.value("online").toBool(), true);
        QCOMPARE(p.value("hasPassword").toBool(), true);
        QCOMPARE(p.value("status").toObject().value("state").toString(), QString("idle"));
        QVERIFY(!p.contains("password"));
        QCOMPARE(call("PUT", "/api/printers/" + id, QJsonDocument(QJsonObject{{"name", "Renamed"}, {"host", fake.host()}}).toJson()).status, 200);
        p = get("/api/printers").array().first().toObject();
        QCOMPARE(p.value("name").toString(), QString("Renamed"));
        QCOMPARE(p.value("hasPassword").toBool(), true);  // kept when the field is left empty
        QCOMPARE(call("DELETE", "/api/printers/" + id).status, 200);
        QVERIFY(get("/api/printers").array().isEmpty());
        QCOMPARE(call("DELETE", "/api/printers/" + id).status, 404);
        // saved for the next start
        QFile f(home.filePath("settings.json"));
        QVERIFY(f.open(QIODevice::ReadOnly));
        QVERIFY(QJsonDocument::fromJson(f.readAll()).object().value("printers").toArray().isEmpty());
    }

    void printerValidation() {
        QCOMPARE(post("/api/printers", {{"host", ""}}).json().value("code").toString(), QString("no_host"));
        QCOMPARE(post("/api/printers", {{"host", "1.2.3.4"}, {"password", "123456789"}}).json().value("code").toString(), QString("password_length"));
        QCOMPARE(post("/api/printers", {{"host", "1.2.3.4"}, {"driver", "nope"}}).json().value("code").toString(), QString("unknown_driver"));
        QCOMPARE(call("POST", "/api/printers", "nope").json().value("code").toString(), QString("bad_request"));
    }

    void editingKeepsMacAndJobOnlyForTheSameAddress() {
        const QJsonObject old{{"id", "a"}, {"name", "R6"}, {"driver", "halot"}, {"host", "10.0.0.5"}, {"password", "pw"},
                              {"mac", "fc:ee:32:02:b5:57"}, {"job", QJsonObject{{"file", "a"}}}};
        const QJsonObject same = Deck::cleanPrinter({{"host", "10.0.0.5"}}, old);
        QCOMPARE(same.value("mac").toString(), QString("fc:ee:32:02:b5:57"));
        QCOMPARE(same.value("job").toObject().value("file").toString(), QString("a"));
        QCOMPARE(same.value("password").toString(), QString("pw"));
        const QJsonObject moved = Deck::cleanPrinter({{"host", "10.0.0.6"}, {"clearPassword", true}}, old);
        QVERIFY(!moved.contains("mac") && !moved.contains("job"));
        QCOMPARE(moved.value("password").toString(), QString());
    }

    void sendAndPrint() {
        FakePrinter fake;
        const QString id = addPrinter(fake);
        Reply r = post("/api/printers/" + id + "/send", {{"path", sliced}, {"print", true}});
        QCOMPARE(r.status, 200);
        QCOMPARE(r.json().value("checkstate").toString(), QString("1"));
        QFile f(sliced);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(fake.files.value("part.cxdlpv4"), f.readAll());
        QCOMPARE(fake.status.value("printStatus").toString(), QString("PRINT_PROCESSING"));
        QCOMPARE(post("/api/printers/" + id + "/send", {{"path", "/etc/passwd"}}).status, 403);
    }

    void printPauseStop() {
        FakePrinter fake;
        const QString id = addPrinter(fake);
        QCOMPARE(post("/api/printers/" + id + "/print", {{"filename", "part.cxdlpv4"}}).status, 200);
        QCOMPARE(deck->printer(id)->status()->state, State::Printing);
        post("/api/printers/" + id + "/pause");
        QCOMPARE(deck->printer(id)->status()->state, State::Paused);
        post("/api/printers/" + id + "/resume");
        post("/api/printers/" + id + "/stop");
        QCOMPARE(deck->printer(id)->status()->state, State::Stopped);
        QCOMPARE(post("/api/printers/" + id + "/print").json().value("code").toString(), QString("bad_request"));
    }

    void printerOffline() {
        QTcpServer free;
        QVERIFY(free.listen(QHostAddress::LocalHost, 0));
        const QString host = QString("127.0.0.1:%1").arg(free.serverPort());
        free.close();
        const QString id = post("/api/printers", {{"host", host}, {"password", "secret1"}}).json().value("id").toString();
        QVERIFY(QTest::qWaitFor([&] { return deck->printer(id)->error() == "no_connection"; }, 5000));
        QCOMPARE(post("/api/printers/" + id + "/stop").json().value("code").toString(), QString("offline"));
    }

    void pairedPhoneCanControlPrints() {
        FakePrinter fake;
        const QString id = addPrinter(fake);
        const auto phone = pair();
        QCOMPARE(post("/api/printers/" + id + "/print", {{"filename", "part.cxdlpv4"}}, phone).status, 200);
        QCOMPARE(fake.status.value("printStatus").toString(), QString("PRINT_PROCESSING"));
    }

    void scanFindsThePrinter() {
        FakePrinter fake;
        // the fake network has one host, 127.0.0.1, which answers on the real HALOT port only in production
        const Reply r = post("/api/printers/scan");
        QCOMPARE(r.status, 200);
        QVERIFY(r.body.startsWith("["));
    }

    void websocketPushesPrinterUpdates() {
        FakePrinter fake;
        QWebSocket ws;
        QStringList messages;
        connect(&ws, &QWebSocket::textMessageReceived, this, [&](const QString &m) { messages << m; });
        ws.open(QUrl(url("/ws").replace("http", "ws")));
        QVERIFY(QTest::qWaitFor([&] { return !messages.isEmpty(); }, 5000));
        QCOMPARE(QJsonDocument::fromJson(messages.first().toUtf8()).object().value("type").toString(), QString("printers"));
        addPrinter(fake);
        QVERIFY(QTest::qWaitFor([&] {
            for (const QString &m : messages)
                if (m.contains("\"online\":true")) return true;
            return false;
        }, 5000));
    }

    void pairedPhoneWebSocketShowsAsOnline() {
        const auto phone = pair();
        QNetworkRequest req(QUrl(url("/ws").replace("http", "ws")));
        for (auto it = phone.begin(); it != phone.end(); ++it) req.setRawHeader(it.key(), it.value());
        QWebSocket ws;
        ws.open(req);
        QVERIFY(QTest::qWaitFor([&] { return ws.state() == QAbstractSocket::ConnectedState; }, 5000));
        QVERIFY(QTest::qWaitFor([&] { return get("/api/remote").json().value("devices").toArray().first().toObject().value("online").toBool(); }, 5000));
        // disconnecting the device closes its connection
        call("DELETE", "/api/remote/devices/" + get("/api/remote").json().value("devices").toArray().first().toObject().value("id").toString());
        QVERIFY(QTest::qWaitFor([&] { return ws.state() == QAbstractSocket::UnconnectedState; }, 5000));
    }

    void quit() {
        QSignalSpy quitting(deck.get(), &Deck::quitRequested);
        QCOMPARE(post("/api/quit").status, 200);
        QVERIFY(quitting.wait(2000));
    }

    void portInUse() {
        Server other(*deck, QString(RESINDECK_SOURCE_DIR) + "/static");
        QVERIFY(!other.listen(server->port(), false));
    }

    // --- helpers -------------------------------------------------------------------------

    void deviceNames() {
        QCOMPARE(deviceName("Mozilla/5.0 (iPhone; CPU iPhone OS 17_0) Safari/604.1"), QString("iPhone · Safari"));
        QCOMPARE(deviceName("Mozilla/5.0 (Linux; Android 14) Chrome/131 Safari/537.36"), QString("Android tablet · Chrome"));
        QCOMPARE(deviceName("Mozilla/5.0 (Windows NT 10.0) Edg/131"), QString("Windows · Edge"));
        QCOMPARE(deviceName(""), QString("Unknown device"));
    }

    void newPinIsSixDigits() {
        for (int i = 0; i < 20; ++i) QVERIFY(QRegularExpression("^\\d{6}$").match(newPin()).hasMatch());
    }

    void mdnsMessages() {
        const auto q = mdns::parseQuery(mdns::query("resindeck.local", 7));
        QVERIFY(q);
        QCOMPARE(q->first, 7);
        QCOMPARE(q->second.first().name, QString("resindeck.local"));
        QCOMPARE(q->second.first().type, 1);
        const auto a = mdns::parseAnswer(mdns::answer("resindeck.local", QHostAddress("192.168.15.13"), 7, true), "resindeck.local");
        QVERIFY(a);
        QCOMPARE(a->toString(), QString("192.168.15.13"));
        QVERIFY(!mdns::parseAnswer(mdns::answer("other.local", QHostAddress("10.0.0.1")), "resindeck.local"));
        QVERIFY(!mdns::parseQuery(mdns::answer("resindeck.local", QHostAddress("10.0.0.1"))));  // not a query
        QVERIFY(!mdns::parseQuery(QByteArray("short")));
        QVERIFY(!mdns::parseQuery(QByteArray::fromHex("000000000001000000000000") + "\x05" "abc"));  // truncated name
    }
};

QTEST_GUILESS_MAIN(TestServer)
#include "test_server.moc"
