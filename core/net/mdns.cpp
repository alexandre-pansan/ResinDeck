#include "net/mdns.h"

#include <QNetworkDatagram>
#include <QNetworkInterface>
#include <QTimer>
#include <QVariant>
#include <QtEndian>

namespace resindeck::mdns {

namespace {

const QHostAddress kGroup("224.0.0.251");
constexpr quint16 kPort = 5353;
constexpr quint16 kTypeA = 1;
constexpr quint16 kTypeAny = 255;
constexpr quint32 kTtl = 120;

quint16 u16(const QByteArray &m, qsizetype at) {
    return at + 2 <= m.size() ? qFromBigEndian<quint16>(m.constData() + at) : 0;
}

void put16(QByteArray &m, quint16 v) {
    char b[2];
    qToBigEndian(v, b);
    m.append(b, 2);
}

void put32(QByteArray &m, quint32 v) {
    char b[4];
    qToBigEndian(v, b);
    m.append(b, 4);
}

void putName(QByteArray &m, const QString &name) {
    for (const QString &label : name.split('.', Qt::SkipEmptyParts)) {
        const QByteArray l = label.toUtf8().left(63);
        m.append(static_cast<char>(l.size()));
        m.append(l);
    }
    m.append('\0');
}

// Reads a possibly compressed name at `pos`; advances `pos` past it. Empty on malformed input.
QString readName(const QByteArray &m, qsizetype &pos) {
    QStringList labels;
    qsizetype p = pos;
    bool jumped = false;
    for (int guard = 0; guard < 64; ++guard) {
        if (p >= m.size()) return {};
        const auto len = static_cast<quint8>(m[p]);
        if (len == 0) {
            if (!jumped) pos = p + 1;
            return labels.join('.');
        }
        if ((len & 0xC0) == 0xC0) {
            if (p + 1 >= m.size()) return {};
            if (!jumped) pos = p + 2;
            jumped = true;
            p = ((len & 0x3F) << 8) | static_cast<quint8>(m[p + 1]);
            continue;
        }
        if (p + 1 + len > m.size()) return {};
        labels << QString::fromUtf8(m.mid(p + 1, len));
        p += 1 + len;
    }
    return {};
}

}  // namespace

std::optional<std::pair<quint16, QList<Question>>> parseQuery(const QByteArray &m) {
    if (m.size() < 12) return std::nullopt;
    const quint16 flags = u16(m, 2);
    if (flags & 0x8000) return std::nullopt;  // a response, not a query
    const quint16 count = u16(m, 4);
    QList<Question> questions;
    qsizetype pos = 12;
    for (quint16 i = 0; i < count; ++i) {
        Question q;
        q.name = readName(m, pos);
        if (q.name.isEmpty() || pos + 4 > m.size()) return std::nullopt;
        q.type = u16(m, pos);
        q.unicast = u16(m, pos + 2) & 0x8000;
        pos += 4;
        questions << q;
    }
    return std::pair{u16(m, 0), questions};
}

QByteArray answer(const QString &name, const QHostAddress &address, quint16 id, bool legacy) {
    QByteArray m;
    put16(m, id);
    put16(m, 0x8400);         // response, authoritative
    put16(m, legacy ? 1 : 0);  // legacy unicast answers repeat the question
    put16(m, 1);
    put16(m, 0);
    put16(m, 0);
    if (legacy) {
        putName(m, name);
        put16(m, kTypeA);
        put16(m, 1);
    }
    putName(m, name);
    put16(m, kTypeA);
    put16(m, legacy ? 1 : 0x8001);  // class IN, with the cache-flush bit for multicast answers
    put32(m, kTtl);
    put16(m, 4);
    put32(m, address.toIPv4Address());
    return m;
}

QByteArray query(const QString &name, quint16 id) {
    QByteArray m;
    put16(m, id);
    put16(m, 0);
    put16(m, 1);
    put16(m, 0);
    put16(m, 0);
    put16(m, 0);
    putName(m, name);
    put16(m, kTypeA);
    put16(m, 1);
    return m;
}

std::optional<QHostAddress> parseAnswer(const QByteArray &m, const QString &name) {
    if (m.size() < 12 || !(u16(m, 2) & 0x8000)) return std::nullopt;
    const quint16 questions = u16(m, 4), answers = u16(m, 6);
    qsizetype pos = 12;
    for (quint16 i = 0; i < questions; ++i) {
        if (readName(m, pos).isEmpty()) return std::nullopt;
        pos += 4;
    }
    for (quint16 i = 0; i < answers; ++i) {
        const QString n = readName(m, pos);
        if (n.isEmpty() || pos + 10 > m.size()) return std::nullopt;
        const quint16 type = u16(m, pos), length = u16(m, pos + 8);
        pos += 10;
        if (type == kTypeA && length == 4 && n.compare(name, Qt::CaseInsensitive) == 0 && pos + 4 <= m.size())
            return QHostAddress(qFromBigEndian<quint32>(m.constData() + pos));
        pos += length;
    }
    return std::nullopt;
}

Responder::Responder(QObject *parent) : QObject(parent) {
    connect(&socket_, &QUdpSocket::readyRead, this, &Responder::readPending);
}

bool Responder::start(const QHostAddress &address) {
    stop();
    address_ = address;
    if (!socket_.bind(QHostAddress::AnyIPv4, kPort, QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) return false;
    QNetworkInterface iface;
    for (const QNetworkInterface &i : QNetworkInterface::allInterfaces())
        for (const QNetworkAddressEntry &e : i.addressEntries())
            if (e.ip() == address) iface = i;
    const bool joined = iface.isValid() ? socket_.joinMulticastGroup(kGroup, iface) : socket_.joinMulticastGroup(kGroup);
    if (!joined) {
        socket_.close();
        return false;
    }
    if (iface.isValid()) socket_.setMulticastInterface(iface);
    socket_.setSocketOption(QAbstractSocket::MulticastTtlOption, QVariant(255));
    socket_.setSocketOption(QAbstractSocket::MulticastLoopbackOption, QVariant(1));  // this computer resolves it too
    running_ = true;
    // announce twice, one second apart (RFC 6762 section 8.3)
    socket_.writeDatagram(answer(kName, address_), kGroup, kPort);
    QTimer::singleShot(1000, this, [this] {
        if (running_) socket_.writeDatagram(answer(kName, address_), kGroup, kPort);
    });
    return true;
}

void Responder::stop() {
    running_ = false;
    if (socket_.state() != QAbstractSocket::UnconnectedState) socket_.close();
}

void Responder::readPending() {
    while (socket_.hasPendingDatagrams()) {
        const QNetworkDatagram d = socket_.receiveDatagram();
        const auto parsed = parseQuery(d.data());
        if (!parsed) continue;
        const auto &[id, questions] = *parsed;
        for (const Question &q : questions) {
            if (q.name.compare(kName, Qt::CaseInsensitive) != 0 || (q.type != kTypeA && q.type != kTypeAny)) continue;
            const bool legacy = d.senderPort() != kPort;  // a plain DNS client: answer it like DNS
            if (legacy || q.unicast)
                socket_.writeDatagram(answer(kName, address_, legacy ? id : quint16(0), legacy), d.senderAddress(),
                                      static_cast<quint16>(d.senderPort()));
            else
                socket_.writeDatagram(answer(kName, address_), kGroup, kPort);
            break;
        }
    }
}

}  // namespace resindeck::mdns
