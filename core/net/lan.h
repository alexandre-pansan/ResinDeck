// Local network helpers: this machine's address, host names, the ARP table (to find printers by MAC
// address) and the network search.
#pragma once

#include "printers/drivers.h"

#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

namespace resindeck::lan {

// The address other devices on the LAN reach this machine at (no packet is sent); empty if none.
QString localIp();
// Every host address of this machine's /24 network.
QStringList localHosts();
// IPv4 address of a host name (an address is returned as is); empty if it does not resolve in time.
QString resolve(const QString &name, int timeoutMs = 2000);
// "host:port" -> "host"
QString hostPart(const QString &host);

// "FC-EE-32-2-B5-57" -> "fc:ee:32:02:b5:57"; empty for empty, broadcast and multicast addresses.
QString normalizeMac(const QString &mac);
// IP -> MAC from `arp -a` (Windows, macOS, BSD) or /proc/net/arp (Linux).
QHash<QString, QString> parseNeighbors(const QString &text);
// This computer's ARP table: the MAC address of every device it talked to recently. Only works for
// devices on the same network segment, which is the case for printers at home.
QHash<QString, QString> neighbors();

// Asks every host whether it is a printer of these families, many at a time.
// Returns {driver id, host} pairs. As a side effect the ARP table learns every device that answered.
QList<std::pair<QString, QString>> sweep(const QList<DriverType> &types, const QStringList &hosts, int timeoutMs = 1500);

// What printers need from the network, so tests can replace it.
class Network {
public:
    virtual ~Network() = default;
    virtual QString resolve(const QString &host) { return lan::resolve(hostPart(host)); }
    virtual QHash<QString, QString> neighbors() { return lan::neighbors(); }
    virtual QStringList hosts() { return localHosts(); }
    virtual bool probe(const DriverType &type, const QString &host) { return type.probe(host, 1500); }
    virtual void sweep(const DriverType &type) { lan::sweep({type}, hosts()); }

    QString macOf(const QString &host) {
        const QString ip = resolve(host);
        return ip.isEmpty() ? QString() : neighbors().value(ip);
    }
    // The address where the printer with this MAC address answers now, other than `exclude`; empty
    // if none. Looks in the ARP table first and searches the network only if needed.
    QString findByMac(const QString &mac, const DriverType &type, const QString &exclude = {});
};

}  // namespace resindeck::lan
