// The HALOT protocol and driver against the fake printer. Ported from the Python version's tests/test_halot.py (legacy branch).
#include "fakeprinter.h"
#include "printers/drivers.h"
#include "printers/halot.h"

#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QRandomGenerator>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>

using namespace resindeck;
using halot::Halot;
using halot::HalotDriver;

namespace {

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

class TestHalotDriver : public QObject {
    Q_OBJECT

    QTemporaryDir dir;
    QString sliced;  // a file to upload; the protocol does not look inside it
    QByteArray slicedData;

private slots:
    void initTestCase() {
        QVERIFY(dir.isValid());
        slicedData.resize(5000);
        QRandomGenerator rng(42);
        rng.fillRange(reinterpret_cast<quint32 *>(slicedData.data()), slicedData.size() / 4);
        slicedData.append("\x12\x34\x56\x80", 4);  // the "CRC": negative as a signed int
        sliced = dir.filePath("part.cxdlpv4");
        QFile f(sliced);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(slicedData);
    }

    // --- protocol ------------------------------------------------------------------------

    void versionNeedsNoPassword() {
        FakePrinter fake;
        Halot h("127.0.0.1", fake.port(), {});
        h.open();
        QCOMPARE(h.request({{"cmd", "VERSION_CHECK"}}).value("version").toString(), QString("1"));
    }

    void status() {
        FakePrinter fake;
        Halot h("127.0.0.1", fake.port(), "secret1");
        h.open();
        QCOMPARE(h.status().value("printStatus").toString(), QString("PRINT_GENERAL"));
        QVERIFY(h.lowerKey);
    }

    void statusFallsBackToTheShortToken() {
        FakePrinter fake;
        fake.shortTokenOnly = true;
        Halot h("127.0.0.1", fake.port(), "secret1");
        h.open();
        QCOMPARE(h.status().value("printStatus").toString(), QString("PRINT_GENERAL"));
        QVERIFY(!h.lowerKey);
    }

    void wrongPassword() {
        FakePrinter fake;
        Halot h("127.0.0.1", fake.port(), "nope123");
        h.open();
        QCOMPARE(errorCode([&] { h.status(); }), QString("wrong_password"));
    }

    void commandsNeedAPassword() {
        FakePrinter fake;
        Halot h("127.0.0.1", fake.port(), {});
        h.open();
        QCOMPARE(errorCode([&] { h.tok(); }), QString("no_password"));
    }

    void binaryNonJsonAndOtherRepliesAreSkipped() {
        FakePrinter fake;
        fake.garbageFirst = true;
        Halot h("127.0.0.1", fake.port(), "secret1");
        h.open();
        QCOMPARE(h.status().value("printStatus").toString(), QString("PRINT_GENERAL"));
    }

    void uploadKey() {
        QCOMPARE(halot::uploadKey(QByteArray("\x00\x00\x00\x80", 4)), QString("-80000000"));
        QCOMPARE(halot::uploadKey(QByteArray("\xff\xff\xff\x7f", 4)), QString("7fffffff"));
        QCOMPARE(halot::uploadKey(QByteArray("xx\x01\x00\x00\x00", 6)), QString("1"));
        QCOMPARE(halot::uploadKey(slicedData), QString("-7fa9cbee"));  // the value Python computes
    }

    // --- upload --------------------------------------------------------------------------

    void uploadCompressed() {
        FakePrinter fake;
        Halot h("127.0.0.1", fake.port(), "secret1");
        h.chunkSize = 512;
        h.open();
        QList<int> seen;
        const QJsonObject r = h.upload(sliced, [&](int pct) { seen.append(pct); });
        QCOMPARE(r.value("checkstate").toString(), QString("1"));
        QCOMPARE(fake.files.value("part.cxdlpv4"), slicedData);
        QVERIFY(std::is_sorted(seen.begin(), seen.end()));
        QCOMPARE(seen.last(), 100);
        const auto start = std::find_if(fake.commands.begin(), fake.commands.end(),
                                        [](const QJsonObject &c) { return c.value("cmd") == "START_FILE"; });
        QCOMPARE(start->value("key").toString(), halot::uploadKey(slicedData));
        QCOMPARE(start->value("size").toString(), QString::number(slicedData.size()));
    }

    void uploadUncompressedWithInlineCheck() {
        FakePrinter fake;
        fake.compress = false;
        fake.checkInline = true;
        Halot h("127.0.0.1", fake.port(), "secret1");
        h.chunkSize = 512;
        h.open();
        h.upload(sliced);
        QCOMPARE(fake.files.value("part.cxdlpv4"), slicedData);
    }

    void uploadErrors_data() {
        QTest::addColumn<QString>("setting");
        QTest::addColumn<QString>("code");
        QTest::addColumn<bool>("cancelled");
        QTest::newRow("busy") << QString("busy") << QString("printer_receiving") << false;
        QTest::newRow("refused") << QString("refuse") << QString("file_refused") << false;
        QTest::newRow("stalled") << QString("stall") << QString("upload_stalled") << true;
        QTest::newRow("rejected") << QString("checkstate") << QString("file_rejected") << true;
    }

    void uploadErrors() {
        QFETCH(QString, setting);
        QFETCH(QString, code);
        QFETCH(bool, cancelled);
        FakePrinter fake;
        fake.busy = setting == "busy";
        fake.refuse = setting == "refuse";
        fake.stall = setting == "stall";
        if (setting == "checkstate") fake.checkstate = 5;
        Halot h("127.0.0.1", fake.port(), "secret1");
        h.chunkSize = 512;
        h.open();
        QCOMPARE(errorCode([&] { h.upload(sliced); }), code);
        QCOMPARE(fake.cancelled, cancelled ? QStringList{"part.cxdlpv4"} : QStringList{});
    }

    void uploadOfAMissingFile() {
        FakePrinter fake;
        Halot h("127.0.0.1", fake.port(), "secret1");
        h.open();
        QCOMPARE(errorCode([&] { h.upload(dir.filePath("nope.cxdlpv4")); }), QString("file_not_found"));
    }

    // --- connection ----------------------------------------------------------------------

    void nothingListening() {
        QTcpServer probe;  // find a free port, then close it
        QVERIFY(probe.listen(QHostAddress::LocalHost, 0));
        const quint16 port = probe.serverPort();
        probe.close();
        Halot h("127.0.0.1", port, {});
        QVERIFY_THROWS_EXCEPTION(NetworkError, h.open(2000));
    }

    void printerGoesAway() {
        auto fake = std::make_unique<FakePrinter>();
        HalotDriver d(fake->host(), "secret1");
        d.connect();
        fake.reset();  // closes the connection
        QVERIFY_THROWS_EXCEPTION(NetworkError, d.status());
    }

    void noAnswerTimesOut() {
        QTcpServer silent;  // accepts the connection and never completes the WebSocket handshake
        QVERIFY(silent.listen(QHostAddress::LocalHost, 0));
        Halot h("127.0.0.1", silent.serverPort(), {});
        QElapsedTimer t;
        t.start();
        QVERIFY_THROWS_EXCEPTION(NetworkError, h.open(300));
        QVERIFY(t.elapsed() < 2000);
    }

    // --- driver --------------------------------------------------------------------------

    void driverSession() {
        FakePrinter fake;
        HalotDriver d(fake.host(), "secret1");
        d.connect();
        QCOMPARE(d.version(), QString("1"));
        d.ping();
        QCOMPARE(d.status().state, State::Idle);
        QCOMPARE(d.upload(sliced).value("checkstate").toString(), QString("1"));
        d.start("part.cxdlpv4");
        QCOMPARE(d.status().state, State::Printing);
        QCOMPARE(d.status().layers, 12);
        d.pause();
        QCOMPARE(d.status().state, State::Paused);
        d.resume();
        QCOMPARE(fake.commands.last().value("cmd").toString(), QString("PRINT_PAUSE"));
        d.stop();
        QCOMPARE(d.status().state, State::Stopped);
        d.close();
    }

    void driverUploadNeedsAPassword() {
        FakePrinter fake;
        HalotDriver d(fake.host(), {});
        d.connect();
        QCOMPARE(errorCode([&] { d.needPassword(); }), QString("no_password"));
        QCOMPARE(errorCode([&] { d.upload(sliced); }), QString("no_password"));
        QVERIFY(fake.files.isEmpty());
    }

    void driverNotConnected() {
        HalotDriver d("127.0.0.1", "secret1");
        QVERIFY_THROWS_EXCEPTION(NetworkError, d.status());
    }

    void probe() {
        FakePrinter fake;
        QVERIFY(HalotDriver::probe(fake.host()));
    }

    void probeWithoutPrinter() {
        QTcpServer free;
        QVERIFY(free.listen(QHostAddress::LocalHost, 0));
        const quint16 port = free.serverPort();
        free.close();
        QVERIFY(!HalotDriver::probe(QString("127.0.0.1:%1").arg(port)));
    }

    void probeSomethingElseOnThePort() {
        QTcpServer http;
        QVERIFY(http.listen(QHostAddress::LocalHost, 0));
        connect(&http, &QTcpServer::newConnection, [&] {
            QTcpSocket *s = http.nextPendingConnection();
            s->write("HTTP/1.1 200 OK\r\n\r\n");
            s->disconnectFromHost();
        });
        QVERIFY(!HalotDriver::probe(QString("127.0.0.1:%1").arg(http.serverPort()), 500));
    }

    void checkPassword() {
        HalotDriver::driverInfo().checkPassword("12345678");
        QCOMPARE(errorCode([] { HalotDriver::driverInfo().checkPassword("123456789"); }), QString("password_length"));
    }

    // --- registry ------------------------------------------------------------------------

    void registry() {
        QCOMPARE(driverType("halot").info.name, QString("Creality HALOT"));
        QCOMPARE(driverType("").info.id, QString("halot"));
        QCOMPARE(errorCode([] { driverType("nope"); }), QString("unknown_driver"));
        FakePrinter fake;
        const DriverType t = driverType("halot");
        QVERIFY(t.probe(fake.host(), 1500));
        auto d = t.create(fake.host(), "secret1");
        d->connect();
        QCOMPARE(d->status().state, State::Idle);
    }

    void everyDriverFillsInItsInfo() {
        for (const DriverType &t : driverTypes()) {
            QVERIFY(!t.info.id.isEmpty() && !t.info.name.isEmpty() && !t.info.extensions.isEmpty());
            for (const QString &ext : t.info.extensions) QVERIFY(ext.startsWith('.') && ext == ext.toLower());
            QVERIFY(t.create && t.probe);
        }
    }

    void describe() {
        const QJsonObject j = HalotDriver::driverInfo().toJson();
        QCOMPARE(j.value("id").toString(), QString("halot"));
        QCOMPARE(j.value("passwordMax").toInt(), 8);
        QCOMPARE(j.value("extensions").toArray().first().toString(), QString(".cxdlpv4"));
    }
};

QTEST_GUILESS_MAIN(TestHalotDriver)
#include "test_halot_driver.moc"
