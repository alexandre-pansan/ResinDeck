// The web interface's HTTP and WebSocket server: the same API as the Python version (server.py),
// so the browser and phone interface in web/ works unchanged.
//
// It listens on the local network so a phone can follow the print: this computer has full access,
// other devices need the PIN. Security checks, for every request:
// - Host: only addresses, localhost, this computer's name and .local names (blocks DNS rebinding);
// - Origin: must match the Host when present (browsers send it on cross-site requests and on every
//   WebSocket handshake, which CORS does not protect);
// - other devices need a session cookie from the PIN, except for /api/session and /api/login;
// - settings (printers, folders, network search, pairing, quit) only from this computer;
// - every response carries a Content-Security-Policy and the usual hardening headers.
#pragma once

#include "deck.h"
#include "net/mdns.h"

#include <QHostAddress>
#include <QHttpHeaders>
#include <QHttpServer>
#include <QHttpServerRequest>
#include <QHttpServerResponder>
#include <QObject>
#include <QRegularExpression>
#include <QTcpServer>
#include <QTimer>
#include <QWebSocket>

#include <functional>
#include <memory>
#include <vector>

namespace resindeck {

inline constexpr auto kSessionCookie = "resindeck_session";

class Server : public QObject {
    Q_OBJECT
public:
    // `webDir` holds index.html and the files served under /static/.
    Server(Deck &deck, QString webDir, QObject *parent = nullptr);
    ~Server() override;

    // Listens on every interface (`lan`) or on 127.0.0.1 only. False if the port is taken.
    bool listen(quint16 port, bool lan = true);
    quint16 port() const;
    bool lan() const { return lan_; }
    bool mdns() const { return mdns_.running(); }
    // Addresses a phone can open, resindeck.local first when it is announced.
    QStringList remoteUrls() const;

    // Paired devices with a live connection, by device id.
    QSet<QString> onlineDevices() const;

    // Whether a request comes from this computer. Tests replace it to act as another device.
    std::function<bool(const QHostAddress &, const QHttpHeaders &)> isLocal;

    // Searches web/index.html's folders: next to the executable, then the source tree.
    static QString findWebDir();

private:
    struct Request;
    using Responder = std::shared_ptr<QHttpServerResponder>;
    using Handler = std::function<void(const Request &, Responder)>;
    struct Route {
        QHttpServerRequest::Method method;
        QRegularExpression pattern;
        Handler handler;
    };

    void addRoutes();
    void route(QHttpServerRequest::Method method, const QString &pattern, Handler handler);
    void handle(const QHttpServerRequest &request, QHttpServerResponder &responder);
    QString guard(const QHttpServerRequest &request, bool local) const;  // empty when allowed, else the reason
    bool authorized(const QHttpServerRequest &request, bool local) const;
    std::optional<QJsonObject> deviceOf(const QHttpHeaders &headers) const;

    void send(Responder r, QHttpServerResponse response) const;
    void sendJson(Responder r, const QJsonValue &json, int status = 200) const;
    void sendError(Responder r, std::exception_ptr error) const;
    void sendFuture(Responder r, QFuture<QJsonObject> future);
    void serveFile(Responder r, const QString &path, bool cache) const;
    QByteArray contentSecurityPolicy() const;

    void onWebSocket();
    void broadcast(const QJsonObject &message, bool localOnly = false);

    Deck &deck_;
    QString webDir_;
    QHttpServer http_;
    QTcpServer *tcp_ = nullptr;  // owned by http_
    bool lan_ = true;
    mdns::Responder mdns_;
    std::vector<Route> routes_;
    mutable QByteArray csp_;

    struct Client {
        std::unique_ptr<QWebSocket> socket;
        bool local = false;
        QString device;
    };
    std::vector<Client> clients_;
    QHash<QString, std::pair<bool, QString>> pendingUpgrades_;  // "address:port" -> {local, device}
    QTimer heartbeat_;
};

}  // namespace resindeck
