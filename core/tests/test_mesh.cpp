// Meshes for the 3D view: the smooth surface through the layers and the voxel faces.
#include "formats/cxdlp.h"
#include "mesh/mesh.h"

#include <QHash>
#include <QTemporaryDir>
#include <QTest>

#include <atomic>
#include <cmath>
#include <cstring>

using namespace resindeck;

namespace {

// 60 x 40 px of 0.1 mm, 12 layers of 0.05 mm; a box at x 20..40 px, y 10..30 px, all layers.
// `edgeGray` paints the column right of the box with that gray (anti-aliasing).
QString writeBox(const QString &path, int edgeGray = 0, int layers = 12) {
    cxdlp::Writer w;
    for (int n = 0; n < layers; ++n) {
        QImage img(60, 40, QImage::Format_Grayscale8);
        img.fill(0);
        for (int y = 10; y < 30; ++y) {
            std::memset(img.scanLine(y) + 20, 255, 20);
            img.scanLine(y)[40] = static_cast<uchar>(edgeGray);
        }
        w.layers.push_back(img);
    }
    w.write(path);
    return path;
}

struct Stats {
    double minX = 1e9, maxX = -1e9, minY = 1e9, maxY = -1e9, minZ = 1e9, maxZ = -1e9, volume = 0;
    bool closed = true, oriented = true;
};

Stats measure(const Mesh &m) {
    Stats s;
    const auto &v = m.vertices;
    for (std::size_t i = 0; i < v.size(); i += 6) {
        s.minX = std::min(s.minX, double(v[i])), s.maxX = std::max(s.maxX, double(v[i]));
        s.minY = std::min(s.minY, double(v[i + 1])), s.maxY = std::max(s.maxY, double(v[i + 1]));
        s.minZ = std::min(s.minZ, double(v[i + 2])), s.maxZ = std::max(s.maxZ, double(v[i + 2]));
    }
    QHash<quint64, int> directed;
    auto key = [](quint32 a, quint32 b) { return (quint64(a) << 32) | b; };
    for (std::size_t t = 0; t < m.indices.size(); t += 3) {
        const quint32 a = m.indices[t], b = m.indices[t + 1], c = m.indices[t + 2];
        for (auto [p, q] : {std::pair{a, b}, {b, c}, {c, a}}) directed[key(p, q)]++;
        const float *A = &v[a * 6ull], *B = &v[b * 6ull], *C = &v[c * 6ull];
        s.volume += (A[0] * (B[1] * C[2] - B[2] * C[1]) - A[1] * (B[0] * C[2] - B[2] * C[0]) + A[2] * (B[0] * C[1] - B[1] * C[0])) / 6.0;
    }
    // closed and consistently oriented: every directed edge once, and its reverse once
    for (auto it = directed.begin(); it != directed.end(); ++it) {
        if (it.value() != 1) s.oriented = false;
        if (!directed.contains(key(quint32(it.key() & 0xffffffff), quint32(it.key() >> 32)))) s.closed = false;
    }
    return s;
}

}  // namespace

class TestMesh : public QObject {
    Q_OBJECT
    QTemporaryDir dir;

private slots:
    void surfaceOfABox() {
        const cxdlp::File f(writeBox(dir.filePath("box.cxdlpv4")));
        const Mesh m = surfaceMesh(f);
        QVERIFY(!m.empty());
        QCOMPARE(m.step, 1);
        QCOMPARE(m.layerStep, 1);
        const Stats s = measure(m);
        QVERIFY(s.closed);
        QVERIFY(s.oriented);
        // the box: x 2.0..4.0 mm, y 1.0..3.0 mm (flipped from rows 10..30 on a 4 mm plate), z 0..0.6 mm,
        // within the half-pixel the surface sits between samples
        QVERIFY2(std::abs(s.minX - 2.0) < 0.06 && std::abs(s.maxX - 4.0) < 0.06, qPrintable(QString("%1 %2").arg(s.minX).arg(s.maxX)));
        QVERIFY2(std::abs(s.minY - 1.0) < 0.06 && std::abs(s.maxY - 3.0) < 0.06, qPrintable(QString("%1 %2").arg(s.minY).arg(s.maxY)));
        QVERIFY2(std::abs(s.minZ) < 0.03 && std::abs(s.maxZ - 0.6) < 0.03, qPrintable(QString("%1 %2").arg(s.minZ).arg(s.maxZ)));
        // outward normals give a positive volume: 2 x 2 x 0.6 mm (corners are slightly cut)
        QVERIFY2(s.volume > 2.4 * 0.85 && s.volume < 2.4 * 1.05, qPrintable(QString::number(s.volume)));
        QVERIFY(std::abs(m.top - 0.6) < 0.03);
    }

    void normalsPointOutward() {
        const Mesh m = surfaceMesh(cxdlp::File(writeBox(dir.filePath("box.cxdlpv4"))));
        int top = 0;
        for (std::size_t i = 0; i < m.vertices.size(); i += 6)
            if (m.vertices[i + 2] > 0.59f && m.vertices[i] > 2.5f && m.vertices[i] < 3.5f && m.vertices[i + 1] > 1.5f && m.vertices[i + 1] < 2.5f) {
                QVERIFY(m.vertices[i + 5] > 0.9f);  // the middle of the top face faces up
                ++top;
            }
        QVERIFY(top > 0);
    }

    void antiAliasingMovesTheEdge() {
        // a gray column right of the box moves its right side by a fraction of a pixel
        const double plain = measure(surfaceMesh(cxdlp::File(writeBox(dir.filePath("a.cxdlpv4"), 0)))).maxX;
        const double gray = measure(surfaceMesh(cxdlp::File(writeBox(dir.filePath("b.cxdlpv4"), 200)))).maxX;
        QVERIFY2(gray > plain + 0.02 && gray < plain + 0.1, qPrintable(QString("%1 %2").arg(plain).arg(gray)));
    }

    void budgetCoarsensTheGrid() {
        const cxdlp::File f(writeBox(dir.filePath("box.cxdlpv4"), 0, 40));
        SurfaceOptions o;
        o.sampleBudget = 20000;
        const Mesh m = surfaceMesh(f, o);
        QVERIFY(m.step > 1 || m.layerStep > 1);
        const Stats s = measure(m);
        QVERIFY(s.closed && s.oriented && s.volume > 0);
    }

    void triangleLimitCoarsensTheGrid() {
        const cxdlp::File f(writeBox(dir.filePath("box.cxdlpv4"), 0, 40));
        const Mesh fine = surfaceMesh(f);
        SurfaceOptions o;
        o.maxTriangles = fine.indices.size() / 3 / 4;
        const Mesh coarse = surfaceMesh(f, o);
        QVERIFY2(coarse.indices.size() / 3 <= o.maxTriangles, qPrintable(QString::number(coarse.indices.size() / 3)));
        QVERIFY(coarse.step > fine.step || coarse.layerStep > fine.layerStep);
        QVERIFY(measure(coarse).closed);
    }

    void printedBoxIsSixRectangles() {
        // straight walls merge across all the layers: 4 walls, the top and the bottom
        const Mesh m = printedMesh(cxdlp::File(writeBox(dir.filePath("box.cxdlpv4"))));
        QCOMPARE(m.indices.size() / 3, std::size_t(12));
        const Stats s = measure(m);
        QVERIFY(std::abs(s.minX - 2.0) < 1e-5 && std::abs(s.maxX - 4.0) < 1e-5);
        QVERIFY(std::abs(s.minY - 1.0) < 1e-5 && std::abs(s.maxY - 3.0) < 1e-5);
        QVERIFY(std::abs(s.minZ) < 1e-5 && std::abs(s.maxZ - 0.6) < 1e-5);
        QVERIFY2(std::abs(s.volume - 2.4) < 1e-4, qPrintable(QString::number(s.volume)));  // outward normals
        QVERIFY(std::abs(m.top - 0.6) < 1e-5);
        QVERIFY(!m.tooLarge);
    }

    void printedVolumeIsExact() {
        // a stepped pyramid with a hole through it: the volume is the lit pixels times pixel x layer
        cxdlp::Writer w;
        qint64 lit = 0;
        for (int n = 0; n < 10; ++n) {
            QImage img(60, 40, QImage::Format_Grayscale8);
            img.fill(0);
            for (int y = 5 + n; y < 35 - n; ++y)
                for (int x = 10 + n; x < 50 - n; ++x)
                    if (!(x >= 28 && x < 32 && y >= 18 && y < 22)) {
                        img.scanLine(y)[x] = 255;
                        ++lit;
                    }
            img.scanLine(2)[2 + n] = 255;  // and a pixel that moves every layer
            ++lit;
            w.layers.push_back(img);
        }
        w.write(dir.filePath("pyramid.cxdlpv4"));
        const Mesh m = printedMesh(cxdlp::File(dir.filePath("pyramid.cxdlpv4")));
        const double expected = double(lit) * 0.1 * 0.1 * 0.05;
        QVERIFY2(std::abs(measure(m).volume - expected) < 1e-4, qPrintable(QString("%1 %2").arg(measure(m).volume).arg(expected)));
    }

    void printedHalfGrayIsSolid() {
        // gray 200 (above half) is printed: the box is one pixel wider
        const Stats s = measure(printedMesh(cxdlp::File(writeBox(dir.filePath("g.cxdlpv4"), 200))));
        QVERIFY(std::abs(s.maxX - 4.1) < 1e-5);
    }

    void printedOverTheLimitIsTooLarge() {
        SurfaceOptions o;
        o.maxTriangles = 8;
        const Mesh m = printedMesh(cxdlp::File(writeBox(dir.filePath("box.cxdlpv4"))), o);
        QVERIFY(m.tooLarge);
        QVERIFY(m.empty());
    }

    void boundsFromTheVoxelGrid() {
        const cxdlp::File f(writeBox(dir.filePath("box.cxdlpv4")));
        SurfaceOptions o;
        modelBounds(f, f.voxels(2), o);
        QVERIFY(o.pixels[0] < 20 && o.pixels[2] > 40 && o.pixels[1] < 10 && o.pixels[3] > 30);
        const double cropped = measure(surfaceMesh(f, o)).volume, whole = measure(surfaceMesh(f)).volume;
        QVERIFY(std::abs(cropped - whole) < 1e-6);
    }

    void cancelReturnsNothing() {
        const cxdlp::File f(writeBox(dir.filePath("box.cxdlpv4")));
        std::atomic<bool> cancel = true;
        SurfaceOptions o;
        o.cancel = &cancel;
        QVERIFY(surfaceMesh(f, o).empty());
    }

    void emptyFile() {
        cxdlp::Writer w;
        QImage black(60, 40, QImage::Format_Grayscale8);
        black.fill(0);
        w.layers = {black, black};
        w.write(dir.filePath("empty.cxdlpv4"));
        QVERIFY(surfaceMesh(cxdlp::File(dir.filePath("empty.cxdlpv4"))).empty());
    }

    void voxelMeshOfABox() {
        const cxdlp::File f(writeBox(dir.filePath("box.cxdlpv4")));
        const Mesh m = voxelMesh(f.voxels(2));
        QVERIFY(!m.empty());
        const Stats s = measure(m);
        QVERIFY(std::abs(s.minX - 2.0) < 0.21 && std::abs(s.maxX - 4.0) < 0.21);
    }
};

QTEST_GUILESS_MAIN(TestMesh)
#include "test_mesh.moc"
