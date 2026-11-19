#include "config.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

using namespace resindeck;

class TestConfig : public QObject {
    Q_OBJECT

    QTemporaryDir home;

    void write(const QString &path, const QByteArray &text) {
        QDir().mkpath(QFileInfo(path).path());
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(text);
    }

private slots:
    void init() {
        QVERIFY(home.isValid());
        QDir(home.path()).removeRecursively();
        QDir().mkpath(home.path());
        qputenv("HOME", home.path().toLocal8Bit());
        qputenv("USERPROFILE", home.path().toLocal8Bit());
        for (const char *v : {"RESINDECK_HOST", "RESINDECK_PASSWORD", "RESINDECK_DRIVER"}) qunsetenv(v);
    }

    void loadConfigFromFileAndEnvironment() {
        write(configPath(), "# comment\nHOST = 10.0.0.7\nPASSWORD=abc\nnot a setting\n");
        CliConfig c = loadConfig();
        QCOMPARE(c.host, QString("10.0.0.7"));
        QCOMPARE(c.password, QString("abc"));
        QCOMPARE(c.driver, QString("halot"));
        qputenv("RESINDECK_HOST", "10.0.0.8");
        qputenv("RESINDECK_PASSWORD", "env");
        c = loadConfig();
        QCOMPARE(c.host, QString("10.0.0.8"));
        QCOMPARE(c.password, QString("env"));
    }

    void loadConfigWithoutFile() {
        const CliConfig c = loadConfig();
        QVERIFY(c.host.isEmpty() && c.password.isEmpty());
        QCOMPARE(c.driver, QString("halot"));
    }

    void migrateConfig() {
        write(home.filePath(".config/halot-send/config"), "HOST=1.2.3.4\n");
        resindeck::migrateConfig();
        QCOMPARE(loadConfig().host, QString("1.2.3.4"));
        write(home.filePath(".config/halot-send/config"), "HOST=changed\n");
        resindeck::migrateConfig();  // never overwrites an existing config
        QCOMPARE(loadConfig().host, QString("1.2.3.4"));
    }

    void formatStatus() {
        QCOMPARE(resindeck::formatStatus(Status{}), QString("State:     idle"));
        Status st;
        st.state = State::Printing;
        st.code = "PRINT_PROCESSING";
        st.file = "a.cxdlpv4";
        st.layer = 50;
        st.layers = 200;
        st.remaining = 3725;
        const QString text = resindeck::formatStatus(st);
        QVERIFY(text.contains("State:     printing (PRINT_PROCESSING)"));
        QVERIFY(text.contains("File:      a.cxdlpv4"));
        QVERIFY(text.contains("Layer:     50/200 (25%)"));
        QVERIFY(text.contains("Remaining: 1h02min"));
    }
};

QTEST_GUILESS_MAIN(TestConfig)
#include "test_config.moc"
