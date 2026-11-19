// A fake Creality HALOT on 127.0.0.1: speaks the WebSocket protocol of printers/halot.cpp
// (see docs/PROTOCOL.md). Ported from the Python version's tests/fakeprinter.py (legacy branch).
#pragma once

#include "printers/halot.h"

#include <QHash>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QWebSocket>
#include <QWebSocketServer>
#include <QtEndian>

#include <memory>

class FakePrinter {
public:
    QString password = "secret1";
    QString version = "1";
    bool shortTokenOnly = false;  // like HALOT BOX 4.7 on the R6: 8 DES bytes, no second block
    bool compress = true;
    bool busy = false;            // START_FILE answers errorcode 1
    bool refuse = false;          // START_FILE answers another error code
    bool stall = false;           // START_DATA never advances
    bool checkInline = false;     // last chunk answered with CHECK_DATA only
    bool garbageFirst = false;    // send a binary frame and a non-JSON text before each reply
    int checkstate = 1;
    QJsonObject status{{"printStatus", "PRINT_GENERAL"}};
    QHash<QString, QByteArray> files;  // file name -> bytes received
    QList<QJsonObject> commands;       // every JSON command received
    QStringList cancelled;

    FakePrinter() : server_("fake HALOT", QWebSocketServer::NonSecureMode) {
        server_.listen(QHostAddress::LocalHost, 0);
        QObject::connect(&server_, &QWebSocketServer::newConnection, [this] {
            while (QWebSocket *ws = server_.nextPendingConnection()) {
                clients_.emplace_back(ws);
                QObject::connect(ws, &QWebSocket::textMessageReceived, [this, ws](const QString &m) { onText(ws, m); });
                QObject::connect(ws, &QWebSocket::binaryMessageReceived,
                                 [this, ws](const QByteArray &m) { onChunk(ws, m); });
            }
        });
    }

    quint16 port() const { return server_.serverPort(); }
    QString host() const { return QString("127.0.0.1:%1").arg(port()); }

private:
    struct Upload {
        QString name, key;
        qsizetype size = 0;
        QByteArray data;
    };

    bool valid(const QString &tok) const {
        return tok == resindeck::halot::token(password, false) ||
               (!shortTokenOnly && tok == resindeck::halot::token(password, true));
    }

    static void send(QWebSocket *ws, const QJsonObject &obj) {
        ws->sendTextMessage(QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact)));
    }

    void reply(QWebSocket *ws, const QString &cmd, QJsonObject fields = {}) {
        if (garbageFirst) {
            ws->sendBinaryMessage(QByteArray("\x00\x01", 2));
            ws->sendTextMessage("not json");
            send(ws, {{"cmd", "OTHER"}});
        }
        fields.insert("cmd", cmd);
        send(ws, fields);
    }

    void onText(QWebSocket *ws, const QString &msg) {
        const QJsonObject r = QJsonDocument::fromJson(msg.toUtf8()).object();
        commands.append(r);
        const QString cmd = r.value("cmd").toString();
        if (cmd == "VERSION_CHECK") {
            reply(ws, cmd, {{"version", version}});
        } else if (cmd == "GET_PRINT_STATUS") {
            reply(ws, cmd, valid(r.value("token").toString()) ? status : QJsonObject{{"printStatus", "TOKEN_ERROR"}});
        } else if (cmd == "START_FILE") {
            if (busy) {
                reply(ws, cmd, {{"errorcode", "1"}});
            } else if (refuse) {
                reply(ws, cmd, {{"errorcode", "7"}});
            } else {
                upload_ = std::make_unique<Upload>(
                    Upload{r.value("filename").toString(), r.value("key").toString(), r.value("size").toString().toLongLong(), {}});
                reply(ws, cmd, {{"errorcode", "0"}, {"compress", compress}, {"key", upload_->key}});
            }
        } else if (cmd == "CANCEL_FILE") {
            cancelled.append(r.value("filename").toString());
            upload_.reset();
            reply(ws, cmd, {{"errorcode", "0"}});
        } else if (cmd == "START_PRINT" || cmd == "PRINT_PAUSE" || cmd == "PRINT_STOP") {
            if (!valid(r.value("token").toString())) {
                reply(ws, cmd, {{"printStatus", "TOKEN_ERROR"}});
                return;
            }
            if (cmd == "START_PRINT")
                status = {{"printStatus", "PRINT_PROCESSING"}, {"filename", r.value("filename")},
                          {"sliceLayerCount", "12"}, {"curSliceLayer", "1"}, {"printRemainTime", "3700"}};
            else if (cmd == "PRINT_PAUSE")
                status["printStatus"] = "PRINT_PAUSE";
            else
                status = {{"printStatus", "PRINT_STOP"}};
            reply(ws, cmd, {{"errorcode", "0"}});
        } else {
            reply(ws, cmd, {{"errorcode", "99"}});
        }
    }

    void onChunk(QWebSocket *ws, QByteArray msg) {
        if (!upload_) return;
        if (compress) {
            const auto size = qFromBigEndian<quint32>(msg.constData());
            msg = qUncompress(msg);
            Q_ASSERT(msg.size() == static_cast<qsizetype>(size));
        }
        if (!stall) upload_->data += msg;
        const qsizetype received = upload_->data.size();
        const bool done = received >= upload_->size;
        if (!(done && checkInline)) {
            // a reply for another transfer first: the client must skip it
            send(ws, {{"cmd", "START_DATA"}, {"key", "other"}, {"received", "0"}});
            send(ws, {{"cmd", "START_DATA"}, {"key", upload_->key}, {"errorcode", "0"},
                      {"received", QString::number(received)}});
        }
        if (done) {
            files[upload_->name] = upload_->data;
            send(ws, {{"cmd", "CHECK_DATA"}, {"checkstate", QString::number(checkstate)}});
        }
    }

    QWebSocketServer server_;
    std::vector<std::unique_ptr<QWebSocket>> clients_;
    std::unique_ptr<Upload> upload_;
};
