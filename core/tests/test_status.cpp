#include "printers/status.h"

#include <QTest>

using namespace resindeck;

class TestStatus : public QObject {
    Q_OBJECT
private slots:
    void stateNamesRoundTrip() {
        for (auto s : {State::Idle, State::Printing, State::Pausing, State::Paused, State::Stopping, State::Stopped,
                       State::Finishing, State::Finished, State::Failed, State::Unknown})
            QCOMPARE(stateFromName(stateName(s)), s);
        QCOMPARE(stateName(State::Printing), QString("printing"));
        QCOMPARE(stateFromName("something else"), State::Unknown);
    }

    void ready() {
        Status st;
        QVERIFY(st.ready());
        for (auto [state, ready] : {std::pair{State::Finished, true}, {State::Failed, true}, {State::Stopped, true},
                                    {State::Printing, false}, {State::Paused, false}, {State::Unknown, false}}) {
            st.state = state;
            QCOMPARE(st.ready(), ready);
        }
    }

    void jsonLeavesOutTheRawReply() {
        Status st{.state = State::Printing, .file = "a.cxdlpv4", .layer = 3, .layers = 10, .remaining = 60,
                  .code = "PRINT_PROCESSING", .raw = {{"x", 1}}};
        const QJsonObject json = st.toJson();
        QCOMPARE(json.value("state").toString(), QString("printing"));
        QCOMPARE(json.value("layer").toInt(), 3);
        QCOMPARE(json.value("code").toString(), QString("PRINT_PROCESSING"));
        QVERIFY(!json.contains("raw"));
    }

    void printerErrorKeepsCodeAndParams() {
        const PrinterError e("busy", "printer_busy", {{"state", "PRINT_PROCESSING"}});
        QCOMPARE(e.message(), QString("busy"));
        QCOMPARE(e.code(), QString("printer_busy"));
        QCOMPARE(e.params().value("state").toString(), QString("PRINT_PROCESSING"));
    }
};

QTEST_APPLESS_MAIN(TestStatus)
#include "test_status.moc"
