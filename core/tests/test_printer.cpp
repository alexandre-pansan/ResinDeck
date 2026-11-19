// The printer manager against the fake HALOT. Ported from the printer, MAC and job tests in
// the Python version's tests/test_server.py (legacy branch).
#include "fakeprinter.h"
#include "printers/printer.h"

#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <atomic>

using namespace resindeck;

namespace {

const QString kMac = "fc:ee:32:02:b5:57";

// The ARP table is made up; probe() talks to the fake printer's port.
class FakeNetwork : public lan::Network {
public:
    explicit FakeNetwork(quint16 printerPort = 0) : port(printerPort) {}
    quint16 port;
    QHash<QString, QString> table;
    int sweeps = 0;
    QString resolve(const QString &host) override { return lan::hostPart(host); }
    QHash<QString, QString> neighbors() override { return table; }
    bool probe(const DriverType &type, const QString &host) override {
        return type.probe(QString("%1:%2").arg(host).arg(port), 1000);
    }
    void sweep(const DriverType &) override { ++sweeps; }
};

PrinterTiming fast(std::function<double()> clock = {}) { return {50, 50, 60000, std::move(clock)}; }

QJsonObject cfg(const QString &host, const QString &password = "secret1") {
    return {{"id", "a"}, {"name", "R6"}, {"driver", "halot"}, {"host", host}, {"password", password}};
}

template <typename T>
T wait(QFuture<T> f) {
    (void)QTest::qWaitFor([&] { return f.isFinished(); }, 10000);
    return f.result();  // rethrows the command's error
}

template <typename F>
QString errorCode(F &&f) {
    try {
        f();
    } catch (const PrinterError &e) {
        return e.code();
    }
    return {};
}

}  // namespace

class TestPrinter : public QObject {
    Q_OBJECT

    QTemporaryDir dir;
    QString sliced;

private slots:
    void initTestCase() {
        QVERIFY(dir.isValid());
        sliced = dir.filePath("part.cxdlpv4");
        QFile f(sliced);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(QByteArray(3000, 'x') + QByteArray("\x01\x02\x03\x04", 4));
    }

    void connectsAndPolls() {
        FakePrinter fake;
        Printer p(cfg(fake.host()), std::make_shared<FakeNetwork>(), fast());
        QVERIFY(QTest::qWaitFor([&] { return p.online() && p.status().has_value(); }, 5000));
        QCOMPARE(p.status()->state, State::Idle);
        const QJsonObject j = p.toJson();
        QCOMPARE(j.value("online").toBool(), true);
        QCOMPARE(j.value("hasPassword").toBool(), true);
        QCOMPARE(j.value("version").toString(), QString("1"));
        QCOMPARE(j.value("status").toObject().value("state").toString(), QString("idle"));
        QCOMPARE(j.value("driverName").toString(), QString("Creality HALOT"));
        QCOMPARE(j.value("extensions").toArray().first().toString(), QString(".cxdlpv4"));
        QVERIFY(j.value("error").isNull() && j.value("upload").isNull() && j.value("job").isNull());
        // status follows the printer
        fake.status = {{"printStatus", "PRINT_PAUSE"}};
        QVERIFY(QTest::qWaitFor([&] { return p.status() && p.status()->state == State::Paused; }, 5000));
    }

    void withoutPassword() {
        FakePrinter fake;
        Printer p(cfg(fake.host(), {}), std::make_shared<FakeNetwork>(), fast());
        QVERIFY(QTest::qWaitFor([&] { return p.error() == "no_password"; }, 5000));
        QVERIFY(p.online() && !p.status());
        QCOMPARE(p.toJson().value("hasPassword").toBool(), false);
    }

    void wrongPassword() {
        FakePrinter fake;
        Printer p(cfg(fake.host(), "nope123"), std::make_shared<FakeNetwork>(), fast());
        QVERIFY(QTest::qWaitFor([&] { return p.error() == "wrong_password"; }, 5000));
    }

    void offlineAndBack() {
        auto fake = std::make_unique<FakePrinter>();
        const QString host = fake->host();
        Printer p(cfg(host), std::make_shared<FakeNetwork>(), fast());
        QVERIFY(QTest::qWaitFor([&] { return p.online(); }, 5000));
        fake.reset();
        QVERIFY(QTest::qWaitFor([&] { return !p.online(); }, 5000));
        QCOMPARE(p.error(), QString("no_connection"));
        QVERIFY(!p.status());
        QCOMPARE(errorCode([&] { wait(p.command("pause")); }), QString("offline"));
    }

    void unknownDriver() {
        QJsonObject c = cfg("1.2.3.4");
        c.insert("driver", "nope");
        QCOMPARE(errorCode([&] { Printer p(c); }), QString("unknown_driver"));
    }

    void printPauseResumeStop() {
        FakePrinter fake;
        Printer p(cfg(fake.host()), std::make_shared<FakeNetwork>(), fast());
        QVERIFY(QTest::qWaitFor([&] { return p.online() && p.status().has_value(); }, 5000));
        wait(p.command("start", "part.cxdlpv4"));
        QCOMPARE(p.status()->state, State::Printing);  // the command refreshes the status
        wait(p.command("pause"));
        QCOMPARE(p.status()->state, State::Paused);
        wait(p.command("resume"));  // the HALOT toggles with the same command
        QCOMPARE(std::count_if(fake.commands.begin(), fake.commands.end(),
                               [](const QJsonObject &c) { return c.value("cmd") == "PRINT_PAUSE"; }),
                 2);
        wait(p.command("stop"));
        QCOMPARE(p.status()->state, State::Stopped);
        QCOMPARE(errorCode([&] { wait(p.command("explode")); }), QString("bad_request"));
    }

    void commandsNeedAPassword() {
        FakePrinter fake;
        Printer p(cfg(fake.host(), {}), std::make_shared<FakeNetwork>(), fast());
        QVERIFY(QTest::qWaitFor([&] { return p.online(); }, 5000));
        QCOMPARE(errorCode([&] { wait(p.command("stop")); }), QString("no_password"));
    }

    void sendAndPrint() {
        FakePrinter fake;
        Printer p(cfg(fake.host()), std::make_shared<FakeNetwork>(), fast());
        QVERIFY(QTest::qWaitFor([&] { return p.online() && p.status().has_value(); }, 5000));
        QList<int> progress;
        // runs in the printer's thread as the signal is emitted, so it sees every step
        connect(&p, &Printer::changed, &p, [&] {
            const QJsonValue u = p.toJson().value("upload");
            if (u.isObject()) progress << u.toObject().value("pct").toInt();
        }, Qt::DirectConnection);
        const QJsonObject r = wait(p.send(sliced, true));
        QCOMPARE(r.value("checkstate").toString(), QString("1"));
        QVERIFY(r.value("start").isObject());
        QCOMPARE(fake.files.value("part.cxdlpv4").size(), 3004);
        QCOMPARE(fake.status.value("printStatus").toString(), QString("PRINT_PROCESSING"));
        QVERIFY(!progress.isEmpty() && progress.last() == 100);
        QVERIFY(p.toJson().value("upload").isNull());
    }

    void sendRefusals() {
        FakePrinter fake;
        Printer p(cfg(fake.host()), std::make_shared<FakeNetwork>(), fast());
        QVERIFY(QTest::qWaitFor([&] { return p.online() && p.status().has_value(); }, 5000));
        try {
            wait(p.send(dir.filePath("model.stl")));
            QFAIL("no error");
        } catch (const PrinterError &e) {
            QCOMPARE(e.code(), QString("file_not_supported"));
            QCOMPARE(e.params().value("ext").toString(), QString(".stl"));
        }
        fake.status = {{"printStatus", "PRINT_PROCESSING"}};
        QVERIFY(QTest::qWaitFor([&] { return p.status() && !p.status()->ready(); }, 5000));
        QCOMPARE(errorCode([&] { wait(p.send(sliced)); }), QString("printer_busy"));
        QVERIFY(fake.files.isEmpty());
    }

    // --- MAC address ---------------------------------------------------------------------

    void learnsItsMac() {
        FakePrinter fake;
        auto net = std::make_shared<FakeNetwork>(fake.port());
        net->table = {{"127.0.0.1", kMac}};
        Printer p(cfg(fake.host()), net, fast());
        QSignalSpy saved(&p, &Printer::configChanged);
        QVERIFY(QTest::qWaitFor([&] { return p.config().value("mac").toString() == kMac; }, 5000));
        QVERIFY(saved.count() >= 1);
        QCOMPARE(p.toJson().value("mac").toString(), kMac);
    }

    void foundAtItsNewAddress() {
        // the printer moved from 127.0.0.2 (nothing there) to 127.0.0.1
        FakePrinter fake;
        auto net = std::make_shared<FakeNetwork>(fake.port());
        net->table = {{"127.0.0.1", kMac}};
        QJsonObject c = cfg(QString("127.0.0.2:%1").arg(fake.port()));
        c.insert("mac", kMac);
        Printer p(c, net, fast());
        // two failed connections first: instant on Linux, about 2 s each on Windows (loopback refusals)
        QVERIFY(QTest::qWaitFor([&] { return p.online(); }, 20000));
        QCOMPARE(p.config().value("host").toString(), fake.host());
        QCOMPARE(net->sweeps, 0);  // found in the ARP table
    }

    void anotherDeviceAtTheOldAddress() {
        FakePrinter fake;
        auto net = std::make_shared<FakeNetwork>(fake.port());
        net->table = {{"127.0.0.1", "aa:bb:cc:dd:ee:ff"}};
        QJsonObject c = cfg(fake.host());
        c.insert("mac", kMac);
        Printer p(c, net, fast());
        QVERIFY(QTest::qWaitFor([&] { return net->sweeps > 0; }, 5000));  // searched for the right MAC
        QVERIFY(!p.online());
        QCOMPARE(p.config().value("host").toString(), fake.host());
    }

    void networkSearchIsRateLimited() {
        auto net = std::make_shared<FakeNetwork>(1);
        QJsonObject c = cfg("127.0.0.2:1");
        c.insert("mac", kMac);
        Printer p(c, net, fast());
        // the first search comes after two failed connections (about 2 s each on Windows)
        QVERIFY(QTest::qWaitFor([&] { return net->sweeps > 0; }, 20000));
        QTest::qWait(3000);  // more failed connections, but within a minute of the first search
        QCOMPARE(net->sweeps, 1);
    }

    void noSearchWithoutAMac() {
        auto net = std::make_shared<FakeNetwork>(1);
        Printer p(cfg("127.0.0.2:1"), net, fast());
        QTest::qWait(300);
        QCOMPARE(net->sweeps, 0);
    }

    // --- print start and end times -------------------------------------------------------

    void jobStartAndFinishAreNoted() {
        FakePrinter fake;
        std::atomic<double> now = 1000;  // read by the printer's thread
        Printer p(cfg(fake.host()), std::make_shared<FakeNetwork>(), fast([&] { return now.load(); }));
        QVERIFY(QTest::qWaitFor([&] { return p.status().has_value(); }, 5000));
        QVERIFY(!p.config().contains("job"));  // nothing printing
        wait(p.command("start", "a.cxdlpv4"));
        QJsonObject job = p.config().value("job").toObject();
        QCOMPARE(job.value("file").toString(), QString("a.cxdlpv4"));
        QCOMPARE(job.value("started").toDouble(), 1000.0);
        QCOMPARE(job.value("approx").toBool(), false);
        QVERIFY(job.value("ended").isNull());
        now = 1006;
        fake.status = {{"printStatus", "PRINT_COMPLETE"}, {"filename", "a.cxdlpv4"}};
        QVERIFY(QTest::qWaitFor([&] { return !p.config().value("job").toObject().value("ended").isNull(); }, 5000));
        QCOMPARE(p.config().value("job").toObject().value("ended").toDouble(), 1006.0);
        QCOMPARE(p.toJson().value("job").toObject().value("ended").toDouble(), 1006.0);
        now = 5000;
        wait(p.command("start", "a.cxdlpv4"));  // printing the same file again is a new job
        job = p.config().value("job").toObject();
        QCOMPARE(job.value("started").toDouble(), 5000.0);
        QVERIFY(job.value("ended").isNull());
    }

    void jobFoundHalfwayGetsAnEstimatedStart() {
        FakePrinter fake;
        // layer 51 of 100: 50 layers done, 50 left in 3000 s, so it began about 3000 s ago
        fake.status = {{"printStatus", "PRINT_PROCESSING"}, {"filename", "b.cxdlpv4"}, {"curSliceLayer", "51"},
                       {"sliceLayerCount", "100"}, {"printRemainTime", "3000"}};
        Printer p(cfg(fake.host()), std::make_shared<FakeNetwork>(), fast([] { return 10000.0; }));
        QVERIFY(QTest::qWaitFor([&] { return p.config().contains("job"); }, 5000));
        const QJsonObject job = p.config().value("job").toObject();
        QCOMPARE(job.value("started").toDouble(), 7000.0);
        QCOMPARE(job.value("approx").toBool(), true);
    }

    void jobWithoutTimeLeftHasNoStart() {
        FakePrinter fake;
        fake.status = {{"printStatus", "PRINT_PROCESSING"}, {"filename", "b.cxdlpv4"}, {"curSliceLayer", "51"},
                       {"sliceLayerCount", "100"}};
        Printer p(cfg(fake.host()), std::make_shared<FakeNetwork>(), fast());
        QVERIFY(QTest::qWaitFor([&] { return p.config().contains("job"); }, 5000));
        QVERIFY(p.config().value("job").toObject().value("started").isNull());
    }

    void endTimeIsUnknownWhenNotSeenFinishing() {
        // ResinDeck restarted after the print ended: no finish time
        FakePrinter fake;
        fake.status = {{"printStatus", "PRINT_COMPLETE"}, {"filename", "a.cxdlpv4"}};
        QJsonObject c = cfg(fake.host());
        c.insert("job", QJsonObject{{"file", "a.cxdlpv4"}, {"started", 1000}, {"approx", false}, {"ended", QJsonValue::Null}});
        Printer p(c, std::make_shared<FakeNetwork>(), fast());
        QVERIFY(QTest::qWaitFor([&] { return !p.config().value("job").toObject().value("ended").isNull(); }, 5000));
        QCOMPARE(p.config().value("job").toObject().value("ended"), QJsonValue(false));
    }

    void savedJobContinuesAfterARestartAndStopHasNoEnd() {
        FakePrinter fake;
        fake.status = {{"printStatus", "PRINT_PROCESSING"}, {"filename", "a.cxdlpv4"}, {"curSliceLayer", "2"},
                       {"sliceLayerCount", "10"}};
        QJsonObject c = cfg(fake.host());
        c.insert("job", QJsonObject{{"file", "a.cxdlpv4"}, {"started", 1000}, {"approx", false}, {"ended", QJsonValue::Null}});
        Printer p(c, std::make_shared<FakeNetwork>(), fast());
        QVERIFY(QTest::qWaitFor([&] { return p.status().has_value(); }, 5000));
        QTest::qWait(150);
        QCOMPARE(p.config().value("job").toObject().value("started").toDouble(), 1000.0);  // still the saved job
        wait(p.command("stop"));
        QCOMPARE(p.config().value("job").toObject().value("ended"), QJsonValue(false));
    }

    void closesWhilePrinterIsBusy() {
        // destroying a Printer in the middle of an upload must not crash or hang
        FakePrinter fake;
        fake.stall = true;
        auto p = std::make_unique<Printer>(cfg(fake.host()), std::make_shared<FakeNetwork>(), fast());
        QVERIFY(QTest::qWaitFor([&] { return p->online() && p->status().has_value(); }, 5000));
        auto f = p->send(sliced);
        QTest::qWait(50);
        QElapsedTimer t;
        t.start();
        p.reset();
        QVERIFY2(t.elapsed() < 2000, qPrintable(QString::number(t.elapsed())));  // not the 60 s upload timeout
        QVERIFY(f.isFinished());
    }
};

QTEST_GUILESS_MAIN(TestPrinter)
#include "test_printer.moc"
