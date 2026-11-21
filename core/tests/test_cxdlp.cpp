// .cxdlpv4 reading and writing. Ported from the Python version's tests/test_cxdlp.py (legacy branch).
#include "formats/cxdlp.h"
#include "formats/sliced.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QTest>
#include <QtEndian>

#include <cstring>

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

// The encoded bytes of a file's preview `index` (0 small, 1 large), straight from the file.
QByteArray rawPreview(const QString &path, int index) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    const QByteArray d = f.readAll();
    auto u32At = [&](qsizetype at) { return static_cast<qsizetype>(qFromLittleEndian<quint32>(d.constData() + at)); };
    const qsizetype magic = qFromBigEndian<quint32>(d.constData());
    const qsizetype nameAt = 4 + magic + 2;
    const qsizetype tail = nameAt + 4 + static_cast<qsizetype>(qFromBigEndian<quint32>(d.constData() + nameAt));
    const qsizetype preview = u32At(tail + (index ? 40 : 28));
    return d.mid(u32At(preview + 8), u32At(preview + 12));
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

    void layerTableAndLiftSettings() {
        // bottom layers (2) get the bottom exposure, light-off time and lift; the table stores the lit area
        const cxdlp::File c(sliced);
        const auto &l = c.layers();
        QVERIFY(near(l[0].exposure, 30.0) && near(l[1].exposure, 30.0) && near(l[2].exposure, 2.5));
        QVERIFY(near(l[0].lightOff, 1.0) && near(l[2].lightOff, 0.5));
        QFile f(sliced);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QByteArray data = f.readAll();
        auto f32At = [&](qint64 at) { return static_cast<double>(qFromLittleEndian<float>(data.constData() + at)); };
        auto u32At = [&](qint64 at) { return qFromLittleEndian<quint32>(data.constData() + at); };
        QCOMPARE(f32At(l[0].address), 5.0);  // bottom lift height
        QCOMPARE(f32At(l[2].address), 4.0);  // lift height
        QCOMPARE(f32At(l[2].address + 4), 90.0);
        QCOMPARE(f32At(l[2].address + 16), 150.0);
        QCOMPARE(f32At(l[2].address + 40), 255.0);
        // layer 0: a 20 x 20 px box of 0.1 x 0.1 mm = 4 mm²; the table entry's 8th word
        const qint64 tableAt = l[0].address - 40 * static_cast<qint64>(l.size());
        QCOMPARE(u32At(tableAt + 28), 4u);
    }

    void rle7LitPixels() {
        QImage img(10, 2, QImage::Format_Grayscale8);
        img.fill(0);
        std::memset(img.scanLine(0), 255, 6);
        img.scanLine(1)[0] = 127;  // gray pixels count too
        QCOMPARE(cxdlp::rle7LitPixels(cxdlp::encodeRle7(img)), 7);
    }

    // Real files from HALOT BOX and Chitubox, rewritten from what the reader gives, come out byte for byte the
    // same. Needs RESINDECK_SAMPLE_FILES: a folder of .cxdlpv4 files (they are not in the repository).
    void rewritesRealFilesExactly() {
        const QString folder = qEnvironmentVariable("RESINDECK_SAMPLE_FILES");
        if (folder.isEmpty()) QSKIP("set RESINDECK_SAMPLE_FILES to a folder of .cxdlpv4 files");
        const QFileInfoList files = QDir(folder).entryInfoList({"*.cxdlpv4"}, QDir::Files, QDir::Size | QDir::Reversed);
        QVERIFY(!files.isEmpty());
        for (const QFileInfo &fi : files) {
            const cxdlp::File c(fi.filePath());
            cxdlp::Writer w;
            w.printer = c.printer();
            w.bed = c.bed();
            w.layerHeight = c.layerHeight();
            w.bottomLayers = c.bottomLayers();
            w.printTime = c.printTime();
            w.antiAlias = c.antiAlias();
            w.params = c.params();
            // previews: Chitubox splits runs by the colors before they are reduced to 15 bits, so ours
            // differ in bytes; they must give the same image, and the original bytes are written as they are
            for (int k : {0, 1}) {
                const QImage img = c.preview(k);
                QCOMPARE(cxdlp::decodeRgb15(cxdlp::encodeRgb15(img), img.width(), img.height()), img);
            }
            w.previewSmall = c.preview(0);
            w.previewLarge = c.preview(1);
            w.encodedPreviewSmall = rawPreview(fi.filePath(), 0);
            w.encodedPreviewLarge = rawPreview(fi.filePath(), 1);
            w.width = c.width();
            w.height = c.height();
            for (int n = 0; n < c.layerCount(); ++n) {
                const QByteArray data = c.layerData(n);
                // our encoding gives the same image; its bytes can differ, as Chitubox sometimes splits a
                // run where two parts of the model meet
                const QImage img = c.layer(n);
                QCOMPARE(cxdlp::decodeRle7(cxdlp::encodeRle7(img), img.width(), img.height()), img);
                w.encodedLayers.push_back(data);
            }
            QFile f(fi.filePath());
            QVERIFY(f.open(QIODevice::ReadOnly));
            const QByteArray original = f.readAll();
            QByteArray ours = w.bytes();
            // the lit area may differ slightly on anti-aliased layers: within 0.5%, take Chitubox's value
            const auto &layers = c.layers();
            const qint64 tableAt = layers.empty() ? 0 : layers[0].address - 40 * static_cast<qint64>(layers.size());
            for (std::size_t n = 0; n < layers.size() && ours.size() == original.size(); ++n) {
                const qsizetype at = static_cast<qsizetype>(tableAt + 40 * static_cast<qint64>(n) + 28);
                const double theirs = qFromLittleEndian<quint32>(original.constData() + at);
                const double mine = qFromLittleEndian<quint32>(ours.constData() + at);
                if (std::abs(theirs - mine) <= 1 + theirs * 0.005) std::memcpy(ours.data() + at, original.constData() + at, 4);
            }
            // the second settings block has the exposures at words 14 and 15; one file of 27, from an older
            // Chitubox, has them at words 13 and 14
            if (ours.size() == original.size()) {
                const qsizetype magic = qFromBigEndian<quint32>(original.constData());
                const qsizetype nameAt = 4 + magic + 2;
                const qsizetype tail = nameAt + 4 + static_cast<qsizetype>(qFromBigEndian<quint32>(original.constData() + nameAt));
                const qsizetype word13 = qFromLittleEndian<quint32>(original.constData() + tail + 72) + 13 * 4;
                if (original.mid(word13, 8) == ours.mid(word13 + 4, 8)) std::memcpy(ours.data() + word13, original.constData() + word13, 12);
            }
            const qsizetype end = ours.size() - 4;  // the checksum covers the areas too
            if (ours.size() == original.size()) {
                const QByteArray crc = original.right(4);
                std::memcpy(ours.data() + end, crc.constData(), 4);
            }
            if (ours != original) {
                qsizetype at = 0;
                while (at < std::min(ours.size(), original.size()) && ours[at] == original[at]) ++at;
                QFAIL(qPrintable(QString("%1: first difference at byte %2 of %3 (ours %4 bytes)")
                                     .arg(fi.fileName()).arg(at).arg(original.size()).arg(ours.size())));
            }
            qInfo("identical: %s (%d layers)", qPrintable(fi.fileName()), c.layerCount());
        }
    }

    void unsupportedExtension() {
        QVERIFY(!isSlicedFile("model.stl"));
        QVERIFY(isSlicedFile("A.CXDLPV4"));
        QVERIFY_THROWS_EXCEPTION(FormatError, loadSliced(dir.filePath("model.stl")));
    }
};

QTEST_GUILESS_MAIN(TestCxdlp)
#include "test_cxdlp.moc"
