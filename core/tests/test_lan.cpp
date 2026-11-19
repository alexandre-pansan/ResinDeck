// ARP parsing and network helpers. Ported from the Python version's tests/test_lan.py (legacy branch).
#include "net/lan.h"

#include <QHostAddress>
#include <QTest>

using namespace resindeck;

namespace {

const char *kLinux = R"(IP address       HW type     Flags       HW address            Mask     Device
192.168.15.8     0x1         0x2         fc:ee:32:02:b5:57     *        enp6s0
192.168.15.9     0x1         0x0         00:00:00:00:00:00     *        enp6s0
192.168.15.1     0x1         0x2         44:89:6D:41:81:10     *        enp6s0
)";
const char *kWindows = R"(
Interface: 192.168.15.13 --- 0x9
  Internet Address      Physical Address      Type
  192.168.15.8          fc-ee-32-02-b5-57     dynamic
  192.168.15.255        ff-ff-ff-ff-ff-ff     static
  224.0.0.22            01-00-5e-00-00-16     static
)";
const char *kMacOs = R"(? (192.168.15.8) at fc:ee:32:2:b5:57 on en0 ifscope [ethernet]
? (192.168.15.20) at (incomplete) on en0 ifscope [ethernet]
)";

const QString kMac = "fc:ee:32:02:b5:57";

// A network where the ARP table and the printers are made up.
class FakeNetwork : public lan::Network {
public:
    QHash<QString, QString> table;
    QStringList printers;  // hosts where probe() finds a printer
    QStringList sweptFor;
    QHash<QString, QString> afterSweep;  // what the ARP table learns from a sweep

    QString resolve(const QString &host) override { return lan::hostPart(host); }
    QHash<QString, QString> neighbors() override { return table; }
    bool probe(const DriverType &, const QString &host) override { return printers.contains(host); }
    void sweep(const DriverType &type) override {
        sweptFor << type.info.id;
        table.insert(afterSweep);
    }
};

}  // namespace

class TestLan : public QObject {
    Q_OBJECT
private slots:
    void parseLinuxProcArp() {
        const auto t = lan::parseNeighbors(kLinux);
        QCOMPARE(t.size(), 2);
        QCOMPARE(t.value("192.168.15.8"), kMac);
        QCOMPARE(t.value("192.168.15.1"), QString("44:89:6d:41:81:10"));
    }

    void parseWindowsArp() {
        const auto t = lan::parseNeighbors(kWindows);
        QCOMPARE(t.value("192.168.15.8"), kMac);
        QVERIFY(!t.contains("192.168.15.255"));  // broadcast
        QVERIFY(!t.contains("224.0.0.22"));      // multicast
        QVERIFY(!t.contains("192.168.15.13"));   // the interface line has no MAC
    }

    void parseMacOsArp() {
        const auto t = lan::parseNeighbors(kMacOs);
        QCOMPARE(t.size(), 1);
        QCOMPARE(t.value("192.168.15.8"), kMac);  // pads "2" to "02"
    }

    void normalizeMac() {
        QCOMPARE(lan::normalizeMac("FC-EE-32-2-B5-57"), kMac);
        QVERIFY(lan::normalizeMac("").isEmpty());
        QVERIFY(lan::normalizeMac("00:00:00:00:00:00").isEmpty());
        QVERIFY(lan::normalizeMac("01:00:5e:00:00:16").isEmpty());
        QVERIFY(lan::normalizeMac("fc:ee:32").isEmpty());
    }

    void hostPartAndResolve() {
        QCOMPARE(lan::hostPart("10.0.0.5:18188"), QString("10.0.0.5"));
        QCOMPARE(lan::hostPart("10.0.0.5"), QString("10.0.0.5"));
        QCOMPARE(lan::resolve("10.1.2.3"), QString("10.1.2.3"));
        QCOMPARE(lan::resolve("localhost"), QString("127.0.0.1"));
        QVERIFY(lan::resolve("no-such-host.invalid", 2000).isEmpty());
    }

    void localIpIsAnIpv4AddressOrEmpty() {
        const QString ip = lan::localIp();
        QVERIFY(ip.isEmpty() || QHostAddress(ip).protocol() == QAbstractSocket::IPv4Protocol);
        if (!ip.isEmpty()) QCOMPARE(lan::localHosts().size(), 254);
    }

    void neighborsReadsTheSystemTable() {
        // the real table: just check it parses without failing
        for (auto it : lan::neighbors().asKeyValueRange()) QVERIFY(!lan::normalizeMac(it.second).isEmpty());
    }

    void findByMacInTheArpTable() {
        FakeNetwork net;
        net.table = {{"10.0.0.9", kMac}};
        net.printers = {"10.0.0.9"};
        QCOMPARE(net.findByMac(kMac, driverType("halot"), "10.0.0.5"), QString("10.0.0.9"));
        QVERIFY(net.sweptFor.isEmpty());
    }

    void findByMacAfterASweep() {
        FakeNetwork net;
        net.afterSweep = {{"10.0.0.7", kMac}};
        net.printers = {"10.0.0.7"};
        QCOMPARE(net.findByMac(kMac, driverType("halot"), "10.0.0.5"), QString("10.0.0.7"));
        QCOMPARE(net.sweptFor, QStringList{"halot"});
    }

    void findByMacSkipsTheOldAddressAndNonPrinters() {
        FakeNetwork net;
        net.table = {{"10.0.0.5", kMac}, {"10.0.0.6", kMac}};
        net.printers = {"10.0.0.5"};  // only the old address answers
        QVERIFY(net.findByMac(kMac, driverType("halot"), "10.0.0.5").isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestLan)
#include "test_lan.moc"
