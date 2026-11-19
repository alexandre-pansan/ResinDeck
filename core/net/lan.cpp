#include "net/lan.h"

#include <QEventLoop>
#include <QFile>
#include <QHostAddress>
#include <QHostInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QTimer>
#include <QUdpSocket>
#include <QtConcurrent/QtConcurrentMap>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace resindeck::lan {

QString localIp() {
    QUdpSocket s;
    s.connectToHost(QHostAddress("10.255.255.255"), 1);
    const QHostAddress a = s.localAddress();
    return a.protocol() == QAbstractSocket::IPv4Protocol && !a.isLoopback() && !a.isNull() ? a.toString() : QString();
}

QStringList localHosts() {
    const QString ip = localIp();
    if (ip.isEmpty()) return {};
    const QString prefix = ip.left(ip.lastIndexOf('.') + 1);
    QStringList hosts;
    for (int i = 1; i < 255; ++i) hosts << prefix + QString::number(i);
    return hosts;
}

QString hostPart(const QString &host) {
    const qsizetype colon = host.lastIndexOf(':');
    return colon > 0 && host.indexOf(':') == colon ? host.left(colon) : host;
}

QString resolve(const QString &name, int timeoutMs) {
    const QHostAddress literal(name);
    if (!literal.isNull()) return literal.toString();
    QString result;
    QEventLoop loop;
    QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);
    const int id = QHostInfo::lookupHost(name, &loop, [&](const QHostInfo &info) {
        for (const QHostAddress &a : info.addresses())
            if (a.protocol() == QAbstractSocket::IPv4Protocol) {
                result = a.toString();
                break;
            }
        loop.quit();
    });
    loop.exec();
    QHostInfo::abortHostLookup(id);
    return result;
}

QString normalizeMac(const QString &mac) {
    if (mac.isEmpty()) return {};
    QStringList parts = mac.toLower().split(QRegularExpression("[:-]"));
    if (parts.size() != 6) return {};
    for (QString &p : parts) p = p.rightJustified(2, '0');
    const QString out = parts.join(':');
    bool ok = false;
    const int first = parts[0].toInt(&ok, 16);
    if (!ok || out == "00:00:00:00:00:00" || (first & 1)) return {};  // empty, broadcast or multicast
    return out;
}

QHash<QString, QString> parseNeighbors(const QString &text) {
    static const QRegularExpression macRe("([0-9a-f]{1,2}(?:[:-][0-9a-f]{1,2}){5})", QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression ipRe(R"((\d{1,3}(?:\.\d{1,3}){3}))");
    QHash<QString, QString> table;
    for (const QString &line : text.split('\n')) {
        const auto ip = ipRe.match(line), mac = macRe.match(line);
        if (!ip.hasMatch() || !mac.hasMatch()) continue;
        if (line.contains(" 0x0 ")) continue;  // /proc/net/arp: incomplete entry
        const QString m = normalizeMac(mac.captured(1));
        if (!m.isEmpty()) table.insert(ip.captured(1), m);
    }
    return table;
}

QHash<QString, QString> neighbors() {
    QFile proc("/proc/net/arp");
    if (proc.open(QIODevice::ReadOnly)) return parseNeighbors(QString::fromLatin1(proc.readAll()));
    QProcess arp;
#ifdef Q_OS_WIN
    arp.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) { args->flags |= CREATE_NO_WINDOW; });
#endif
    arp.start("arp", {"-a"});
    if (!arp.waitForFinished(5000)) return {};
    return parseNeighbors(QString::fromLocal8Bit(arp.readAllStandardOutput()));
}

QList<std::pair<QString, QString>> sweep(const QList<DriverType> &types, const QStringList &hosts, int timeoutMs) {
    QList<std::pair<QString, QString>> jobs;  // {driver id, host}
    for (const DriverType &t : types)
        for (const QString &h : hosts) jobs.append({t.info.id, h});
    QThreadPool pool;
    pool.setMaxThreadCount(64);
    const QList<bool> found = QtConcurrent::blockingMapped(&pool, jobs, [timeoutMs](const std::pair<QString, QString> &job) {
        return driverType(job.first).probe(job.second, timeoutMs);
    });
    QList<std::pair<QString, QString>> result;
    for (qsizetype i = 0; i < jobs.size(); ++i)
        if (found[i]) result.append(jobs[i]);
    return result;
}

QString Network::findByMac(const QString &mac, const DriverType &type, const QString &exclude) {
    auto candidates = [&] {
        QStringList ips;
        const auto table = neighbors();
        for (auto it = table.begin(); it != table.end(); ++it)
            if (it.value() == mac && it.key() != exclude) ips << it.key();
        return ips;
    };
    QStringList ips = candidates();
    if (ips.isEmpty()) {
        sweep(type);  // fills the ARP table
        ips = candidates();
    }
    for (const QString &ip : ips)
        if (probe(type, ip)) return ip;
    return {};
}

}  // namespace resindeck::lan
