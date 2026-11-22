// The window with a fake HALOT: following a print in 3D.
#include "fakeprinter.h"
#include "formats/cxdlp.h"
#include "mainwindow.h"

#include <QApplication>
#include <QDir>
#include <QJsonArray>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QTemporaryDir>
#include <QTest>

#include <cstring>

using namespace resindeck;

namespace {

class FakeNetwork : public lan::Network {
public:
    QString resolve(const QString &host) override { return lan::hostPart(host); }
    QHash<QString, QString> neighbors() override { return {}; }
    QStringList hosts() override { return {"127.0.0.1"}; }
};

QJsonObject printing(const QString &file, int layer) {
    return {{"printStatus", "PRINT_PROCESSING"}, {"filename", file}, {"curSliceLayer", QString::number(layer)},
            {"sliceLayerCount", "12"}, {"printRemainTime", "100"}};
}

}  // namespace

class TestDesktop : public QObject {
    Q_OBJECT

    QTemporaryDir home;
    QString lib, part;
    std::unique_ptr<FakePrinter> fake;
    std::unique_ptr<Deck> deck;
    std::unique_ptr<ui::MainWindow> window;
    QString id;

    QSlider *slider() { return window->findChild<QSlider *>("layerSlider"); }
    QLabel *badge() { return window->findChild<QLabel *>("badge"); }
    QPushButton *followLink() { return window->findChild<QPushButton *>("link"); }
    bool following() { return !badge()->isHidden(); }

private slots:
    void init() {
        QVERIFY(home.isValid());
        QDir(home.path()).removeRecursively();
        lib = home.filePath("lib");
        QDir().mkpath(lib);
        qputenv("RESINDECK_CONFIG_DIR", home.filePath("config").toLocal8Bit());
        part = lib + "/part.cxdlpv4";
        cxdlp::Writer w;
        for (int n = 0; n < 12; ++n) {
            QImage img(60, 40, QImage::Format_Grayscale8);
            img.fill(0);
            for (int y = 10; y < 30; ++y) std::memset(img.scanLine(y) + 20, 255, 20);
            w.layers.push_back(img);
        }
        w.write(part);
        QFile settings(home.filePath("settings.json"));
        QVERIFY(settings.open(QIODevice::WriteOnly));
        settings.write(QJsonDocument(QJsonObject{{"printers", QJsonArray{}}, {"folders", QJsonArray{lib}}}).toJson());
        settings.close();

        fake = std::make_unique<FakePrinter>();
        deck = std::make_unique<Deck>(home.filePath("settings.json"), std::make_shared<FakeNetwork>(),
                                      PrinterTiming{50, 50, 60000, {}}, home.filePath("cache"));
        id = deck->addPrinter({{"name", "R6"}, {"host", fake->host()}, {"password", "secret1"}});
        QVERIFY(QTest::qWaitFor([this] { return deck->printer(id)->online() && deck->printer(id)->status(); }, 5000));
        window = std::make_unique<ui::MainWindow>(*deck, nullptr);
        window->show();
    }

    void cleanup() {
        window.reset();
        deck.reset();
        fake.reset();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }

    void followsWhenThePrintStartsAfterTheSliderWasMoved() {
        window->openFile(part);
        QTest::qWait(200);
        slider()->setValue(2);  // looking at the layers by hand
        QVERIFY(!following());
        fake->status = printing("part.cxdlpv4", 5);
        QTRY_VERIFY_WITH_TIMEOUT(following(), 5000);
        QTRY_COMPARE(slider()->value(), 4);  // layer 5
        fake->status = printing("part.cxdlpv4", 7);
        QTRY_COMPARE(slider()->value(), 6);
    }

    void movingTheSliderStopsUntilTheNextPrint() {
        window->openFile(part);
        fake->status = printing("part.cxdlpv4", 5);
        QTRY_VERIFY_WITH_TIMEOUT(following(), 5000);
        slider()->setValue(1);
        QVERIFY(!following());
        fake->status = printing("part.cxdlpv4", 8);
        QTest::qWait(300);
        QVERIFY(!following());
        QCOMPARE(slider()->value(), 1);
        // the print ends and the file is printed again: followed again
        fake->status = {{"printStatus", "PRINT_COMPLETE"}};
        QTRY_VERIFY_WITH_TIMEOUT(ui::printerReady(deck->printer(id)->toJson()), 5000);
        fake->status = printing("part.cxdlpv4", 1);
        QTRY_VERIFY_WITH_TIMEOUT(following(), 5000);
    }

    void theEndOfThePrintShowsTheWholeModel() {
        window->openFile(part);
        fake->status = printing("part.cxdlpv4", 5);
        QTRY_VERIFY_WITH_TIMEOUT(following(), 5000);
        fake->status = {{"printStatus", "PRINT_COMPLETE"}};
        QTRY_VERIFY_WITH_TIMEOUT(!following(), 5000);
        QCOMPARE(slider()->value(), slider()->maximum());
    }

    void followLinkOnlyForFilesInTheLibrary() {
        fake->status = printing("other.cxdlpv4", 5);
        QTRY_VERIFY_WITH_TIMEOUT(deck->printer(id)->toJson().value("status").toObject().value("file") == "other.cxdlpv4", 5000);
        QTest::qWait(100);
        QVERIFY(followLink()->isHidden());
        fake->status = printing("part.cxdlpv4", 5);
        QTRY_VERIFY_WITH_TIMEOUT(!followLink()->isHidden(), 5000);
        QTest::mouseClick(followLink(), Qt::LeftButton);
        QTRY_VERIFY(following());
        QVERIFY(followLink()->isHidden());  // no link while following
    }
};

QTEST_MAIN(TestDesktop)
#include "test_desktop.moc"
