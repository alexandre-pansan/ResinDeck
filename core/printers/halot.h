// Creality HALOT resin printers (tested on the HALOT-R6): WebSocket on port 18188 with JSON
// commands. Based on the MIT-licensed https://github.com/mrtska/HalotMageProSharp plus what
// HALOT BOX 4.7 does on the HALOT-R6. Details in docs/PROTOCOL.md.
//
// `Halot` speaks the protocol; `HalotDriver` adapts it to the Driver contract (printers/driver.h).
#pragma once

#include "printers/driver.h"
#include "printers/status.h"

#include <QJsonObject>
#include <QQueue>
#include <QString>

#include <memory>

class QWebSocket;

namespace resindeck::halot {

inline constexpr quint16 kPort = 18188;
inline constexpr int kPasswordMax = 8;
inline constexpr qsizetype kChunk = 1 << 20;  // upload chunk size, before compression

// The token the printer expects in every command: the password, PKCS#7-padded and encrypted with
// single DES under a fixed key, in base64. HalotMageProSharp appends a second fixed block for
// passwords over 4 characters; HALOT BOX 4.7 (R6) was seen sending only the 8 DES bytes, so both
// forms are tried. Throws PrinterError("password_length") unless the password is 1–8 ASCII characters.
QString token(const QString &password, bool lowerKey = true);

// The printer's GET_PRINT_STATUS reply in the normalized form. Numbers may come as strings.
Status parseStatus(const QJsonObject &reply);

// The upload key: the file's trailing CRC (last 4 bytes) read as a signed little-endian int, in hex
// with a minus sign when negative, as HALOT BOX sends it.
QString uploadKey(const QByteArray &data);

// One WebSocket connection to a HALOT. Blocking: each call waits for the printer's answer.
class Halot {
public:
    Halot(QString host, quint16 port, QString password);
    ~Halot();
    Halot(const Halot &) = delete;
    Halot &operator=(const Halot &) = delete;

    void open(int timeoutMs = 10000);
    void close();

    // Send a command and wait for the reply with the same "cmd" (or `want`).
    QJsonObject request(const QJsonObject &command, const QString &want = {}, int timeoutMs = 10000);
    // GET_PRINT_STATUS, trying both token forms; throws PrinterError("wrong_password").
    QJsonObject status();
    // Protocol as done by HALOT BOX 4.7 (cxWebsocket.dll):
    // - START_FILE with the file name, size and upload key;
    // - data in 1 MiB chunks, each waiting for its START_DATA ack ("received" so far);
    // - when START_FILE answers compress=true, every chunk goes through qCompress
    //   (big-endian uncompressed length, then zlib);
    // - CHECK_DATA ends it; on the HALOT-R6 checkstate 1 means the file was accepted.
    // A single uncompressed frame is silently ignored by the printer (received stays 0).
    QJsonObject upload(const QString &path, const Driver::Progress &progress = {}, int timeoutMs = 60000);

    // The token for the current password. Throws PrinterError("no_password") without one.
    QString tok() const;
    bool lowerKey = true;  // which token form the printer accepted
    qsizetype chunkSize = kChunk;

private:
    void sendJson(const QJsonObject &obj);
    QJsonObject receive(int timeoutMs);
    QJsonObject receiveAny(const QStringList &commands, int timeoutMs);
    void cancelUpload(const QString &filename, const QString &key);

    QString host_;
    quint16 port_;
    QString password_;
    std::unique_ptr<QWebSocket> ws_;
    QQueue<QJsonObject> inbox_;
    bool closed_ = false;
};

class HalotDriver : public Driver {
public:
    // `host` may end in ":port" (tests use another port).
    HalotDriver(const QString &host, QString password);
    ~HalotDriver() override;

    static const DriverInfo &driverInfo();
    const DriverInfo &info() const override { return driverInfo(); }

    void connect() override;
    void close() override;
    void ping() override;
    Status status() override;
    QJsonObject upload(const QString &path, const Progress &progress = {}) override;
    QJsonObject start(const QString &filename) override;
    QJsonObject pause() override;
    QJsonObject resume() override;
    QJsonObject stop() override;

    // True if a HALOT answers at `host` (VERSION_CHECK, no password needed). Quick, for network search.
    static bool probe(const QString &host, int timeoutMs = 1500);

private:
    QJsonObject command(QJsonObject command);
    Halot &halot();

    std::unique_ptr<Halot> halot_;
};

}  // namespace resindeck::halot
