// Answers mDNS (multicast DNS, RFC 6762) queries for resindeck.local with this computer's address, so
// phones on the same network can open http://resindeck.local:8765. Only that one name and A
// (IPv4 address) records: nothing else is announced.
#pragma once

#include <QByteArray>
#include <QHostAddress>
#include <QObject>
#include <QString>
#include <QUdpSocket>

#include <optional>

namespace resindeck::mdns {

inline constexpr auto kName = "resindeck.local";

struct Question {
    QString name;
    quint16 type = 0;
    bool unicast = false;  // the QU bit: answer to the sender directly
};

// Parsing and building of DNS messages, exposed for tests.
std::optional<std::pair<quint16, QList<Question>>> parseQuery(const QByteArray &message);  // {id, questions}
QByteArray answer(const QString &name, const QHostAddress &address, quint16 id = 0, bool legacy = false);
QByteArray query(const QString &name, quint16 id = 0);
std::optional<QHostAddress> parseAnswer(const QByteArray &message, const QString &name);

class Responder : public QObject {
    Q_OBJECT
public:
    explicit Responder(QObject *parent = nullptr);
    // Starts answering for kName with `address`; false if port 5353 cannot be used.
    bool start(const QHostAddress &address);
    void stop();
    bool running() const { return running_; }

private:
    void readPending();

    QUdpSocket socket_;
    QHostAddress address_;
    bool running_ = false;
};

}  // namespace resindeck::mdns
