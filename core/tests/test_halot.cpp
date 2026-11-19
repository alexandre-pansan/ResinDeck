#include "printers/halot.h"

#include <QTest>

using namespace resindeck;

Q_DECLARE_METATYPE(resindeck::State)

class TestHalot : public QObject {
    Q_OBJECT
private slots:
    // Reference values from the Python version (drivers/halot.py, legacy branch), which the printer accepts.
    void tokenMatchesThePythonVersion_data() {
        QTest::addColumn<QString>("password");
        QTest::addColumn<QString>("withLowerKey");
        QTest::addColumn<QString>("shortToken");
        QTest::newRow("a") << QString("a") << QString("yDsTd3KDH50=") << QString("yDsTd3KDH50=");
        QTest::newRow("abc") << QString("abc") << QString("E/8A0f6OHM4=") << QString("E/8A0f6OHM4=");
        QTest::newRow("abcd") << QString("abcd") << QString("u51y6dxGPJ0=") << QString("u51y6dxGPJ0=");
        QTest::newRow("abcde") << QString("abcde") << QString("0dvqm0ZkM9ysR7ACiBVuXA==") << QString("0dvqm0ZkM9w=");
        QTest::newRow("secret1") << QString("secret1") << QString("Qxbcqf3F4aisR7ACiBVuXA==") << QString("Qxbcqf3F4ag=");
        QTest::newRow("12345678") << QString("12345678") << QString("uUR3lPKfxcGsR7ACiBVuXA==") << QString("uUR3lPKfxcE=");
        QTest::newRow("symbols") << QString("Ab3$%^&*") << QString("gDXhRCQHnIysR7ACiBVuXA==") << QString("gDXhRCQHnIw=");
    }

    void tokenMatchesThePythonVersion() {
        QFETCH(QString, password);
        QFETCH(QString, withLowerKey);
        QFETCH(QString, shortToken);
        QCOMPARE(halot::token(password), withLowerKey);
        QCOMPARE(halot::token(password, false), shortToken);
    }

    void tokenRejectsBadPasswords_data() {
        QTest::addColumn<QString>("password");
        QTest::newRow("empty") << QString();
        QTest::newRow("too long") << QString("123456789");
        QTest::newRow("not ascii") << QString::fromUtf8("senhá");
    }

    void tokenRejectsBadPasswords() {
        QFETCH(QString, password);
        try {
            halot::token(password);
            QFAIL("no error");
        } catch (const PrinterError &e) {
            QCOMPARE(e.code(), QString("password_length"));
            QCOMPARE(e.params().value("max").toInt(), 8);
        }
    }

    void parseStatusStates_data() {
        QTest::addColumn<QString>("code");
        QTest::addColumn<State>("state");
        QTest::newRow("general") << QString("PRINT_GENERAL") << State::Idle;
        QTest::newRow("empty") << QString("") << State::Idle;
        QTest::newRow("processing") << QString("PRINT_PROCESSING") << State::Printing;
        QTest::newRow("pause") << QString("PRINT_PAUSE") << State::Paused;
        QTest::newRow("stoping") << QString("PRINT_STOPING") << State::Stopping;
        QTest::newRow("complete") << QString("PRINT_COMPLETE") << State::Finished;
        QTest::newRow("fail") << QString("PRINT_FAIL") << State::Failed;
        QTest::newRow("new") << QString("PRINT_SOMETHING_NEW") << State::Unknown;
    }

    void parseStatusStates() {
        QFETCH(QString, code);
        QFETCH(State, state);
        const Status st = halot::parseStatus({{"printStatus", code}});
        QCOMPARE(st.state, state);
        QCOMPARE(st.code, code);
    }

    void parseStatusFields() {
        const Status st = halot::parseStatus({{"printStatus", "PRINT_PROCESSING"}, {"filename", "a.cxdlpv4"},
                                              {"sliceLayerCount", "200"}, {"curSliceLayer", "50"},
                                              {"printRemainTime", 3725}});
        QCOMPARE(st.file, QString("a.cxdlpv4"));
        QCOMPARE(st.layer, 50);
        QCOMPARE(st.layers, 200);
        QCOMPARE(st.remaining, 3725);  // a number, not a string
        QVERIFY(!st.ready());
        QCOMPARE(st.raw.value("filename").toString(), QString("a.cxdlpv4"));
        QCOMPARE(halot::parseStatus({{"curSliceLayer", "?"}}).layer, 0);
        QCOMPARE(halot::parseStatus({}).state, State::Idle);  // no printStatus at all
    }
};


QTEST_APPLESS_MAIN(TestHalot)
#include "test_halot.moc"
