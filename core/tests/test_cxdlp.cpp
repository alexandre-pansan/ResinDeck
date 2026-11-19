// .cxdlpv4 reading and writing. Ported from the Python version's tests/test_cxdlp.py (legacy branch).
#include "formats/cxdlp.h"
#include "formats/sliced.h"

#include <QFile>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QTest>

using namespace resindeck;

namespace {

// A box at the plate centre that gets one pixel wider every layer (as the Python version's tests/cxdlp_writer.py).
std::vector<QImage> layerImages(int resX = 60, int resY = 40, int count = 12) {
    std::vector<QImage> images;
    for (int n = 0; n < count; ++n) {
        QImage img(resX, resY, QImage::Format_Grayscale8);
        img.fill(0);
        for (int y = 10; y < 30; ++y)
            for (int x = 20 - n / 2; x < 40 + n / 2; ++x) img.scanLine(y)[x] = 255;
        images.push_back(img);
    }
    return images;
}

// The file stores 32-bit floats.
bool near(double a, double b) { return std::abs(a - b) < 1e-5; }

bool sameMask(const QImage &a, const QImage &b) {  // equal above/below the 127 threshold
    if (a.size() != b.size()) return false;
    for (int y = 0; y < a.height(); ++y)
        for (int x = 0; x < a.width(); ++x)
            if ((a.constScanLine(y)[x] > 127) != (b.constScanLine(y)[x] > 127)) return false;
    return true;
}

}  // namespace

class TestCxdlp : public QObject {
    Q_OBJECT

    QTemporaryDir dir;
    QString sliced;

    QString write(const QString &name, cxdlp::Writer w) {
        if (w.layers.empty()) w.layers = layerImages();
        const QString path = dir.filePath(name);
        w.write(path);
        return path;
    }

private slots:
    void initTestCase() {
        QVERIFY(dir.isValid());
        sliced = write("part.cxdlpv4", {});
    }

    void loadHeader() {
        const cxdlp::File c(sliced);
        QCOMPARE(c.printer(), QString("HALOT-R6"));
        QCOMPARE(c.width(), 60);
        QCOMPARE(c.height(), 40);
        QCOMPARE(c.bed()[0], 6.0);
        QCOMPARE(c.bed()[2], 50.0);
        QVERIFY(near(c.layerHeight(), 0.05));
        QCOMPARE(c.layerCount(), 12);
        QVERIFY(near(c.layerZ(11), 0.6));
        QVERIFY(near(c.pixelMm()[0], 0.1));
        QVERIFY(!c.pixelPair());
    }

    void info() {
        const QJsonObject i = loadSliced(sliced)->info();
        QCOMPARE(i.value("file").toString(), QString("part.cxdlpv4"));
        QCOMPARE(i.value("layerCount").toInt(), 12);
        QCOMPARE(i.value("printTime").toInt(), 1234);
        QCOMPARE(i.value("antiAlias").toInt(), 4);
        QCOMPARE(i.value("exposure").toDouble(), 2.5);
        QCOMPARE(i.value("bottomExposure").toDouble(), 30.0);
        QCOMPARE(i.value("volumeMl").toDouble(), 1.25);
        QCOMPARE(i.value("previews").toInt(), 2);
        QCOMPARE(i.value("resolution").toArray(), (QJsonArray{60, 40}));
    }

    void previewRoundTrip() {
        const QImage img = cxdlp::File(sliced).preview();
        QCOMPARE(img.size(), QSize(16, 12));
        QCOMPARE(img.pixelColor(0, 0), QColor(248, 0, 0));
        QCOMPARE(img.pixelColor(15, 11), QColor(0, 0, 248));
        QVERIFY(cxdlp::File(sliced).preview(0) == img);
    }

    void layerRoundTrip_data() {
        QTest::addColumn<int>("n");
        for (int n : {0, 5, 11}) QTest::newRow(qPrintable(QString::number(n))) << n;
    }

    void layerRoundTrip() {
        QFETCH(int, n);
        const QImage got = cxdlp::File(sliced).layer(n);
        QCOMPARE(got.size(), QSize(60, 40));
        QVERIFY(sameMask(got, layerImages()[static_cast<std::size_t>(n)]));
    }

    void longRunsUseMultibyteStrides() {
        // 600x400 = 240 000 black pixels in one run: needs the 3-byte stride form
        QImage blank(600, 400, QImage::Format_Grayscale8);
        blank.fill(0);
        QImage big = blank.copy();
        for (int y = 100; y < 300; ++y) std::memset(big.scanLine(y) + 200, 255, 200);
        for (const QImage &img : {blank, big}) {
            cxdlp::Writer w;
            w.bed = {60.0, 40.0, 50.0};
            w.layers = {img};
            const cxdlp::File c(write("big.cxdlpv4", w));
            QVERIFY(sameMask(c.layer(0), img));
        }
    }

    void voxels() {
        const Voxels v = cxdlp::File(sliced).voxels(2);
        QVERIFY(!v.empty());
        QCOMPARE(v.cells.size(), std::size_t(v.shape[0]) * std::size_t(v.shape[1]) * std::size_t(v.shape[2]));
        QVERIFY(near(v.voxel[0], 0.2));
        QVERIFY(near(v.voxel[1], 0.2));
        QCOMPARE(v.bed[0], 6.0);
        QVERIFY(near(v.origin[1], 1.0));  // the box starts at y = 10 px
        // the box is 20 px tall: 10 cells, all solid in its middle column
        QCOMPARE(v.shape[1], 10);
        for (int y = 0; y < v.shape[1]; ++y) QVERIFY(v.at(0, y, v.shape[2] / 2));
    }

    void voxelsOfAnEmptyFile() {
        QImage black(60, 40, QImage::Format_Grayscale8);
        black.fill(0);
        cxdlp::Writer w;
        w.layers = {black, black, black};
        const Voxels v = cxdlp::File(write("empty.cxdlpv4", w)).voxels(2);
        QVERIFY(v.empty());
        QCOMPARE(v.shape[0], 0);
    }

    void voxelsReportProgress() {
        QList<double> seen;
        cxdlp::File(sliced).voxels(2, [&](double p) { seen.append(p); });
        QVERIFY(!seen.isEmpty());
        for (double p : seen) QVERIFY(p >= 0 && p < 1);
    }

    void notACxdlp() {
        cxdlp::Writer w;
        w.magic = QByteArray("NOTSLICED\0", 10);
        QVERIFY_THROWS_EXCEPTION(FormatError, cxdlp::File(write("x.cxdlpv4", w)));
        QFile junk(dir.filePath("junk.cxdlpv4"));
        QVERIFY(junk.open(QIODevice::WriteOnly));
        junk.write("hello");
        junk.close();
        QVERIFY_THROWS_EXCEPTION(FormatError, cxdlp::File(junk.fileName()));
        QVERIFY_THROWS_EXCEPTION(FormatError, cxdlp::File(dir.filePath("missing.cxdlpv4")));
    }

    void encryptedLayersAreRefused() {
        cxdlp::Writer w;
        w.encrypted = true;
        const cxdlp::File c(write("enc.cxdlpv4", w));
        QVERIFY_THROWS_EXCEPTION(FormatError, c.layer(0));
    }

    void checksum() {
        QVERIFY(cxdlp::verifyChecksum(sliced));
        cxdlp::Writer w;
        w.corruptCrc = true;
        QVERIFY(!cxdlp::verifyChecksum(write("bad.cxdlpv4", w)));
        // what Python's zlib.crc32(data, 0xFFFFFFFF) ^ 0xFFFFFFFF gives
        QCOMPARE(cxdlp::footerCrc("123456789"), 0x2DFD2D88u);
    }

    void pixelPairDecoding() {
        // 4x1 image = 2 units of two pixels; code 0x80 | right << 3 | left, then a run byte
        const QByteArray units("\xb8\x00\x87", 3);  // 0x80 | 7 << 3 | 0, run 0, 0x80 | 0 << 3 | 7
        QVERIFY(cxdlp::isPixelPair(units, 4, 1));
        const QImage img = cxdlp::decodePixelPair(units, 4, 1);
        const uchar *px = img.constScanLine(0);
        QCOMPARE((QList<int>{px[0], px[1], px[2], px[3]}), (QList<int>{0, 255, 255, 0}));
    }

    void pixelPairDetectionRejectsRle7() {
        QVERIFY(!cxdlp::isPixelPair(QByteArray("\x7f\x7f", 2), 2, 1));  // no 0x80 flag
        QVERIFY(!cxdlp::isPixelPair(QByteArray("\x80\x05", 2), 4, 1));  // run longer than the row
        QVERIFY(!cxdlp::isPixelPair(QByteArray("\x80", 1), 3, 1));      // odd width
        QVERIFY(!cxdlp::isPixelPair(QByteArray("\x80", 1), 4, 2));      // does not cover the image
    }

    void unsupportedExtension() {
        QVERIFY(!isSlicedFile("model.stl"));
        QVERIFY(isSlicedFile("A.CXDLPV4"));
        QVERIFY_THROWS_EXCEPTION(FormatError, loadSliced(dir.filePath("model.stl")));
    }
};

QTEST_GUILESS_MAIN(TestCxdlp)
#include "test_cxdlp.moc"
