#include "printers/halot.h"

#include "printers/des.h"

#include <QByteArray>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QJsonValue>
#include <QTimer>
#include <QUrl>
#include <QWebSocket>
#include <QtEndian>

#include <cstdio>

namespace resindeck::halot {

namespace {

constexpr des::Block kDesKey{'a', '8', '5', 'e', '9', 'd', '6', '8'};
constexpr des::Block kLowerKey{0xAC, 0x47, 0xB0, 0x02, 0x88, 0x15, 0x6E, 0x5C};

// The R6 answers checkstate 1 on success; HalotMageProSharp documents 0.
bool checkOk(int checkstate) { return checkstate == 1 || checkstate == 0; }

const QHash<QString, State> &states() {
    static const QHash<QString, State> map{
        {"", State::Idle},
        {"PRINT_GENERAL", State::Idle},
        {"PRINT_PROCESSING", State::Printing},
        {"PRINT_PROGRESS", State::Printing},
        {"PRINT_PRINTING", State::Printing},
        {"PRINT_PAUSING", State::Pausing},
        {"PRINT_PAUSE", State::Paused},
        {"PRINT_STOPING", State::Stopping},  // sic, as the firmware spells it
        {"PRINT_STOPPING", State::Stopping},
        {"PRINT_STOP", State::Stopped},
        {"PRINT_COMPLETING", State::Finishing},
        {"PRINT_COMPLETE", State::Finished},
        {"PRINT_END", State::Finished},
        {"PRINT_FAIL", State::Failed},
    };
    return map;
}

// The printer sends numbers either as JSON numbers or as strings.
int toInt(const QJsonValue &v, int fallback = 0) {
    if (v.isDouble()) return v.toInt();
    bool ok = false;
    const int n = v.toString().toInt(&ok);
    return ok ? n : fallback;
}

// true, "true", 1 or "1"
bool truthy(const QJsonValue &v) { return v.toBool() || v.toString() == "true" || toInt(v) != 0; }

QString compact(const QJsonObject &obj) { return QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact)); }

// "host" or "host:port"
std::pair<QString, quint16> splitHost(const QString &host) {
    const qsizetype colon = host.lastIndexOf(':');
    if (colon > 0 && host.indexOf(':') == colon) {
        bool ok = false;
        const auto port = host.mid(colon + 1).toUShort(&ok);
        if (ok) return {host.left(colon), port};
    }
    return {host, kPort};
}

}  // namespace

QString token(const QString &password, bool lowerKey) {
    bool isAscii = true;
    for (QChar ch : password) isAscii = isAscii && ch.unicode() < 128;
    if (password.isEmpty() || password.size() > kPasswordMax || !isAscii)
        throw PrinterError("The printer password must be 1 to 8 characters long.", "password_length",
                           {{"max", kPasswordMax}});
    const QByteArray ascii = password.toLatin1();

    // PKCS#7 padding to 8 bytes. Only the first block is sent, so an 8-character password is
    // encrypted as it is (its padding is a whole second block).
    des::Block block;
    block.fill(static_cast<std::uint8_t>(8 - ascii.size() % 8));
    for (qsizetype i = 0; i < ascii.size(); ++i) block[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(ascii[i]);

    const des::Block encrypted = des::encrypt(kDesKey, block);
    QByteArray out(reinterpret_cast<const char *>(encrypted.data()), 8);
    if (lowerKey && password.size() > 4) out.append(reinterpret_cast<const char *>(kLowerKey.data()), 8);
    return QString::fromLatin1(out.toBase64());
}

Status parseStatus(const QJsonObject &reply) {
    Status st;
    st.code = reply.value("printStatus").toString();
    st.state = states().value(st.code, State::Unknown);
    st.file = reply.value("filename").toString();
    st.layer = toInt(reply.value("curSliceLayer"));
    st.layers = toInt(reply.value("sliceLayerCount"));
    st.remaining = toInt(reply.value("printRemainTime"));
    st.raw = reply;
    return st;
}

QString uploadKey(const QByteArray &data) {
    if (data.size() < 4) return QStringLiteral("0");
    const auto crc = qFromLittleEndian<qint32>(data.constData() + data.size() - 4);
    return QString::number(static_cast<qint64>(crc), 16);  // "-80000000" style for negatives
}

// --- Halot -----------------------------------------------------------------------------

Halot::Halot(QString host, quint16 port, QString password)
    : host_(std::move(host)), port_(port), password_(std::move(password)) {}

Halot::~Halot() { close(); }

void Halot::open(int timeoutMs) {
    ws_ = std::make_unique<QWebSocket>();
    closed_ = false;
    QObject::connect(ws_.get(), &QWebSocket::textMessageReceived, [this](const QString &msg) {
        QJsonParseError err{};
        const QJsonDocument doc = QJsonDocument::fromJson(msg.toUtf8(), &err);
        if (doc.isObject())
            inbox_.enqueue(doc.object());
        else
            std::fprintf(stderr, "(non-JSON reply) %s\n", qPrintable(msg.left(200)));
    });  // binary messages are not used by the printer and are ignored
    QObject::connect(ws_.get(), &QWebSocket::disconnected, [this] { closed_ = true; });

    QEventLoop loop;
    QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);
    QObject::connect(ws_.get(), &QWebSocket::connected, &loop, &QEventLoop::quit);
    QObject::connect(ws_.get(), &QWebSocket::disconnected, &loop, &QEventLoop::quit);
    QObject::connect(ws_.get(), &QWebSocket::errorOccurred, &loop, &QEventLoop::quit);
    ws_->open(QUrl(QString("ws://%1:%2").arg(host_).arg(port_)));
    loop.exec();
    if (ws_->state() != QAbstractSocket::ConnectedState) {
        const QString why = ws_->error() == QAbstractSocket::UnknownSocketError ? "timed out" : ws_->errorString();
        ws_.reset();
        throw NetworkError(QString("Could not connect to %1:%2 (%3)").arg(host_).arg(port_).arg(why).toStdString());
    }
}

void Halot::close() {
    if (!ws_) return;
    if (ws_->state() == QAbstractSocket::ConnectedState) {
        QEventLoop loop;
        QTimer::singleShot(1000, &loop, &QEventLoop::quit);
        QObject::connect(ws_.get(), &QWebSocket::disconnected, &loop, &QEventLoop::quit);
        ws_->close();
        if (ws_->state() != QAbstractSocket::UnconnectedState) loop.exec();
    }
    ws_.reset();
    inbox_.clear();
}

void Halot::sendJson(const QJsonObject &obj) {
    if (!ws_ || closed_) throw NetworkError("The connection to the printer is closed.");
    ws_->sendTextMessage(compact(obj));
}

QJsonObject Halot::receive(int timeoutMs) {
    QTimer deadline;
    deadline.setSingleShot(true);
    deadline.start(timeoutMs);
    while (inbox_.isEmpty()) {
        if (!ws_ || closed_) throw NetworkError("The printer closed the connection.");
        if (!deadline.isActive()) throw NetworkError("The printer did not answer in time.");
        QEventLoop loop;
        QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
        QObject::connect(ws_.get(), &QWebSocket::textMessageReceived, &loop, &QEventLoop::quit);
        QObject::connect(ws_.get(), &QWebSocket::disconnected, &loop, &QEventLoop::quit);
        loop.exec();
    }
    return inbox_.dequeue();
}

QJsonObject Halot::receiveAny(const QStringList &commands, int timeoutMs) {
    while (true) {
        QJsonObject r = receive(timeoutMs);
        if (commands.contains(r.value("cmd").toString())) return r;
    }
}

QJsonObject Halot::request(const QJsonObject &command, const QString &want, int timeoutMs) {
    sendJson(command);
    const QString expected = want.isEmpty() ? command.value("cmd").toString() : want;
    while (true) {
        QJsonObject r = receive(timeoutMs);
        if (r.value("cmd").toString() == expected || (expected == "VERSION_CHECK" && r.contains("version"))) return r;
    }
}

QString Halot::tok() const {
    if (password_.isEmpty()) throw PrinterError("The printer password is not set.", "no_password");
    return token(password_, lowerKey);
}

QJsonObject Halot::status() {
    QJsonObject r = request({{"cmd", "GET_PRINT_STATUS"}, {"token", tok()}});
    if (r.value("printStatus").toString() == "TOKEN_ERROR" && password_.size() > 4) {
        lowerKey = !lowerKey;
        r = request({{"cmd", "GET_PRINT_STATUS"}, {"token", tok()}});
    }
    if (r.value("printStatus").toString() == "TOKEN_ERROR")
        throw PrinterError("Wrong password (TOKEN_ERROR). Check the network password on the printer screen.",
                           "wrong_password");
    return r;
}

QJsonObject Halot::upload(const QString &path, const Driver::Progress &progress, int timeoutMs) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        throw PrinterError(QString("Cannot read %1: %2").arg(path, file.errorString()), "file_not_found",
                           {{"path", path}});
    const QByteArray data = file.readAll();
    const qsizetype size = data.size();
    const QString name = QFileInfo(path).fileName();
    const QString key = uploadKey(data);

    sendJson({{"cmd", "START_FILE"}, {"filename", name}, {"key", key}, {"offset", "0"},
              {"size", QString::number(size)}});
    QJsonObject r = receiveAny({"START_FILE"}, 30000);
    const int error = toInt(r.value("errorcode"));
    if (error == 1)
        throw PrinterError("The printer is still receiving another file. Cancel that upload (or restart the printer) "
                           "and try again.",
                           "printer_receiving");
    if (error != 0)
        throw PrinterError("The printer refused the file: " + compact(r), "file_refused", {{"reply", compact(r)}});
    const bool compress = truthy(r.value("compress"));

    try {
        int last = -1;
        qint64 prev = -1;
        int stalled = 0;
        for (qsizetype offset = 0; offset < size; offset += chunkSize) {
            QByteArray chunk = data.mid(offset, chunkSize);
            if (compress) chunk = qCompress(chunk);  // the same framing the printer expects
            ws_->sendBinaryMessage(chunk);
            do {
                r = receiveAny({"START_DATA", "CHECK_DATA"}, timeoutMs);
            } while (r.contains("key") && r.value("key").toString() != key);  // a reply for another transfer
            if (r.value("cmd").toString() == "CHECK_DATA") break;
            if (toInt(r.value("errorcode")) != 0)
                throw PrinterError("Upload error: " + compact(r), "upload_error", {{"reply", compact(r)}});
            const qint64 received = toInt(r.value("received"));
            stalled = received <= prev ? stalled + 1 : 0;
            prev = std::max(prev, received);
            if (stalled >= 3)
                throw PrinterError(QString("The printer stopped accepting data (%1 of %2 bytes received).")
                                       .arg(received)
                                       .arg(size),
                                   "upload_stalled", {{"received", received}, {"size", size}});
            const auto pct = static_cast<int>(received * 100 / size);
            if (pct != last && progress) {
                last = pct;
                progress(pct);
            }
        }
        if (r.value("cmd").toString() != "CHECK_DATA") r = receiveAny({"CHECK_DATA"}, timeoutMs);
        const int checkstate = toInt(r.value("checkstate"), -1);
        if (!checkOk(checkstate))
            throw PrinterError(
                QString("The printer rejected the uploaded file (checkstate=%1).").arg(r.value("checkstate").toVariant().toString()),
                "file_rejected", {{"checkstate", r.value("checkstate").toVariant()}});
        return r;
    } catch (...) {
        cancelUpload(name, key);
        throw;
    }
}

void Halot::cancelUpload(const QString &filename, const QString &key) {
    try {
        sendJson({{"cmd", "CANCEL_FILE"}, {"key", key}, {"filename", filename}});
        receiveAny({"CANCEL_FILE"}, 5000);
    } catch (const std::exception &) {
        // the connection may be gone already; the printer drops the transfer when it reconnects
    }
}

// --- HalotDriver -----------------------------------------------------------------------

HalotDriver::HalotDriver(const QString &host, QString password) : Driver(host, std::move(password)) {}

HalotDriver::~HalotDriver() = default;

const DriverInfo &HalotDriver::driverInfo() {
    static const DriverInfo info{
        .id = "halot",
        .name = "Creality HALOT",
        .models = "HALOT-R6 (tested); other HALOT models with network printing should work",
        .extensions = {".cxdlpv4", ".cxdlp"},
        .password = true,
        .passwordMax = kPasswordMax,
    };
    return info;
}

Halot &HalotDriver::halot() {
    if (!halot_) throw NetworkError("Not connected to the printer.");
    return *halot_;
}

void HalotDriver::connect() {
    const auto [address, port] = splitHost(host_);
    halot_ = std::make_unique<Halot>(address, port, password_);
    halot_->open();
    version_ = halot_->request({{"cmd", "VERSION_CHECK"}}).value("version").toVariant().toString();
}

void HalotDriver::close() {
    if (halot_) halot_->close();
    halot_.reset();
}

void HalotDriver::ping() { halot().request({{"cmd", "VERSION_CHECK"}}); }

Status HalotDriver::status() { return parseStatus(halot().status()); }

QJsonObject HalotDriver::upload(const QString &path, const Progress &progress) {
    halot().tok();  // fail before sending anything when the password is missing
    const QJsonObject r = halot().upload(path, progress);
    return {{"checkstate", r.value("checkstate")}};
}

QJsonObject HalotDriver::command(QJsonObject cmd) {
    cmd.insert("token", halot().tok());
    return halot().request(cmd);
}

QJsonObject HalotDriver::start(const QString &filename) {
    return command({{"cmd", "START_PRINT"}, {"filename", filename}});
}

QJsonObject HalotDriver::pause() { return command({{"cmd", "PRINT_PAUSE"}}); }

QJsonObject HalotDriver::resume() { return command({{"cmd", "PRINT_PAUSE"}}); }  // the same command toggles

QJsonObject HalotDriver::stop() { return command({{"cmd", "PRINT_STOP"}}); }

bool HalotDriver::probe(const QString &host, int timeoutMs) {
    const auto [address, port] = splitHost(host);
    try {
        Halot h(address, port, {});
        h.open(timeoutMs);
        return h.request({{"cmd", "VERSION_CHECK"}}, {}, timeoutMs).contains("version");
    } catch (const std::exception &) {
        return false;
    }
}

}  // namespace resindeck::halot
