// resindeck-cli run as a separate process against the fake printer. Ported from the Python version's tests/test_cli.py (legacy branch).
#include "fakeprinter.h"

#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QProcess>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

class TestCli : public QObject {
    Q_OBJECT

    QTemporaryDir home;
    QString sliced;
    QByteArray slicedData;

    struct Result {
        int code;
        QString out, err;
    };

    // Runs the CLI while this process keeps answering as the fake printer.
    Result run(const QStringList &args, const QByteArray &stdinText = "n\n", const QString &password = "secret1") {
        QProcess p;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert("HOME", home.path());
        env.insert("USERPROFILE", home.path());
        env.insert("RESINDECK_PASSWORD", password);
        env.remove("RESINDECK_HOST");
        env.remove("RESINDECK_DRIVER");
        p.setProcessEnvironment(env);
        QEventLoop loop;
        connect(&p, &QProcess::finished, &loop, &QEventLoop::quit);
        QTimer::singleShot(20000, &loop, &QEventLoop::quit);
        p.start(RESINDECK_CLI, args);
        p.write(stdinText);
        p.closeWriteChannel();
        loop.exec();
        if (p.state() != QProcess::NotRunning) {
            p.kill();
            p.waitForFinished();
        }
        return {p.exitCode(), QString::fromUtf8(p.readAllStandardOutput()), QString::fromUtf8(p.readAllStandardError())};
    }

private slots:
    void initTestCase() {
        QVERIFY(home.isValid());
        slicedData = QByteArray(3000, 'x') + QByteArray("\x01\x02\x03\x04", 4);
        sliced = home.filePath("part.cxdlpv4");
        QFile f(sliced);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(slicedData);
    }

    void noHost() {
        const Result r = run({"status"});
        QCOMPARE(r.code, 1);
        QVERIFY(r.err.contains("No printer IP"));
    }

    void hostFromTheConfigFile() {
        FakePrinter fake;
        QDir().mkpath(home.filePath(".config/resindeck"));
        QFile f(home.filePath(".config/resindeck/config"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(("HOST=" + fake.host() + "\n").toUtf8());
        f.close();
        const Result r = run({"version"});
        QFile::remove(f.fileName());
        QCOMPARE(r.code, 0);
        QVERIFY2(r.out.contains("Creality HALOT at " + fake.host()), qPrintable(r.out + r.err));
    }

    void version() {
        FakePrinter fake;
        const Result r = run({"--host", fake.host(), "version"}, {}, {});
        QCOMPARE(r.code, 0);
        QVERIFY2(r.out.contains("Creality HALOT at " + fake.host() + ", firmware 1"), qPrintable(r.out + r.err));
    }

    void status() {
        FakePrinter fake;
        Result r = run({"--host", fake.host(), "status"});
        QCOMPARE(r.code, 0);
        QVERIFY(r.out.contains("State:     idle (PRINT_GENERAL)"));
        r = run({"--host", fake.host(), "raw-status"});
        QVERIFY(r.out.contains("\"printStatus\": \"PRINT_GENERAL\""));
    }

    void wrongPassword() {
        FakePrinter fake;
        const Result r = run({"--host", fake.host(), "status"}, {}, "wrong12");
        QCOMPARE(r.code, 1);
        QVERIFY(r.err.contains("TOKEN_ERROR"));
    }

    void unreachablePrinter() {
        QTcpServer free;
        QVERIFY(free.listen(QHostAddress::LocalHost, 0));
        const QString host = QString("127.0.0.1:%1").arg(free.serverPort());
        free.close();
        const Result r = run({"--host", host, "version"});
        QCOMPARE(r.code, 1);
        QVERIFY2(r.err.contains("Could not talk to " + host), qPrintable(r.err));
    }

    void sendAndPrint() {
        FakePrinter fake;
        const Result r = run({"--host", fake.host(), "send", sliced, "--print"}, "y\n");
        QCOMPARE(r.code, 0);
        QVERIFY2(r.out.contains("Upload finished. {\"checkstate\":\"1\"}") && r.out.contains("100%"), qPrintable(r.out));
        QCOMPARE(fake.files.value("part.cxdlpv4"), slicedData);
        QCOMPARE(fake.status.value("printStatus").toString(), QString("PRINT_PROCESSING"));
    }

    void sendWithoutConfirmationDoesNotPrint() {
        FakePrinter fake;
        QCOMPARE(run({"--host", fake.host(), "send", sliced, "--print"}, "n\n").code, 0);
        QCOMPARE(fake.status.value("printStatus").toString(), QString("PRINT_GENERAL"));
    }

    void sendChecksTheFile() {
        FakePrinter fake;
        Result r = run({"--host", fake.host(), "send", home.filePath("nope")});
        QCOMPARE(r.code, 1);
        QVERIFY(r.err.contains("File not found"));
        QFile stl(home.filePath("model.stl"));
        QVERIFY(stl.open(QIODevice::WriteOnly));
        stl.write(QByteArray("solid x").repeated(10));
        stl.close();
        fake.status = {{"printStatus", "PRINT_PROCESSING"}};
        r = run({"--host", fake.host(), "send", stl.fileName()});
        QVERIFY2(r.err.contains("expect .cxdlpv4") && r.err.contains("printing (PRINT_PROCESSING)"), qPrintable(r.err));
    }

    void printPauseStop() {
        FakePrinter fake;
        QCOMPARE(run({"--host", fake.host(), "print", "part.cxdlpv4", "-y"}).code, 0);
        QCOMPARE(fake.status.value("printStatus").toString(), QString("PRINT_PROCESSING"));
        run({"--host", fake.host(), "pause"});
        QCOMPARE(fake.status.value("printStatus").toString(), QString("PRINT_PAUSE"));
        run({"--host", fake.host(), "pause"});  // resumes (the HALOT toggles)
        QCOMPARE(fake.commands.last().value("cmd").toString(), QString("PRINT_PAUSE"));
        run({"--host", fake.host(), "stop"});
        QCOMPARE(fake.status.value("printStatus").toString(), QString("PRINT_STOP"));
    }

    void unknownDriverAndCommand() {
        Result r = run({"--host", "127.0.0.1", "--driver", "nope", "version"});
        QCOMPARE(r.code, 1);
        QVERIFY(r.err.contains("Unknown printer type"));
        r = run({"--host", "127.0.0.1", "explode"});
        QCOMPARE(r.code, 1);
        QVERIFY(r.err.contains("Unknown command"));
    }

    void noCommand() {
        const Result r = run({});
        QCOMPARE(r.code, 1);
        QVERIFY(r.err.contains("No command"));
    }
};

QTEST_GUILESS_MAIN(TestCli)
#include "test_cli.moc"
