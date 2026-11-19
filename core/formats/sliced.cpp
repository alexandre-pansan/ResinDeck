#include "formats/sliced.h"

#include "formats/cxdlp.h"

#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QtEndian>

#include <algorithm>
#include <cmath>

namespace resindeck {

std::array<double, 2> SlicedFile::pixelMm() const {
    const auto b = bed();
    return {b[0] / width(), b[1] / height()};
}

Voxels SlicedFile::voxels(int step, const Progress &progress) const {
    const auto [px, py] = pixelMm();
    const int k = std::max(1, static_cast<int>(std::lround(step * px / std::max(layerHeight(), 1e-3))));
    const int nx = width() / step, ny = height() / step;
    const int nz = (layerCount() + k - 1) / k;

    // Full grid first, then crop to the model's bounding box.
    std::vector<std::uint8_t> full(static_cast<std::size_t>(nz) * static_cast<std::size_t>(ny) * static_cast<std::size_t>(nx));
    int lo[3] = {nz, ny, nx}, hi[3] = {-1, -1, -1};
    for (int j = 0; j < nz; ++j) {
        const QImage img = layer(j * k);
        for (int y = 0; y < ny * step; ++y) {
            const uchar *row = img.constScanLine(y);
            for (int x = 0; x < nx * step; ++x) {
                if (row[x] <= 127) continue;
                const int cy = y / step, cx = x / step;
                full[(static_cast<std::size_t>(j) * static_cast<std::size_t>(ny) + static_cast<std::size_t>(cy)) *
                         static_cast<std::size_t>(nx) +
                     static_cast<std::size_t>(cx)] = 1;
                lo[0] = std::min(lo[0], j), hi[0] = std::max(hi[0], j);
                lo[1] = std::min(lo[1], cy), hi[1] = std::max(hi[1], cy);
                lo[2] = std::min(lo[2], cx), hi[2] = std::max(hi[2], cx);
            }
        }
        if (progress && j % 20 == 0) progress(double(j) / nz);
    }

    Voxels v;
    v.bed = bed();
    if (hi[0] < 0) return v;  // nothing exposed
    v.shape = {hi[0] - lo[0] + 1, hi[1] - lo[1] + 1, hi[2] - lo[2] + 1};
    v.cells.reserve(static_cast<std::size_t>(v.shape[0]) * static_cast<std::size_t>(v.shape[1]) * static_cast<std::size_t>(v.shape[2]));
    for (int z = lo[0]; z <= hi[0]; ++z)
        for (int y = lo[1]; y <= hi[1]; ++y) {
            const auto start = full.begin() + static_cast<std::ptrdiff_t>((static_cast<std::size_t>(z) * static_cast<std::size_t>(ny) + static_cast<std::size_t>(y)) *
                                                                              static_cast<std::size_t>(nx) +
                                                                          static_cast<std::size_t>(lo[2]));
            v.cells.insert(v.cells.end(), start, start + v.shape[2]);
        }
    v.voxel = {step * px, step * py, k * layerHeight()};
    v.origin = {lo[2] * step * px, lo[1] * step * py, lo[0] * k * layerHeight()};
    v.layersPerVoxel = k;
    return v;
}

Voxels decodeVoxels(const QByteArray &payload) {
    Voxels v;
    if (payload.size() < 4) return v;
    const auto metaSize = static_cast<qsizetype>(qFromLittleEndian<quint32>(payload.constData()));
    if (4 + metaSize > payload.size()) return v;
    const QJsonObject meta = QJsonDocument::fromJson(payload.mid(4, metaSize)).object();
    const QJsonArray shape = meta.value("shape").toArray();
    if (shape.size() != 3) return v;
    auto triple = [&](const char *key) {
        const QJsonArray a = meta.value(key).toArray();
        return std::array<double, 3>{a.at(0).toDouble(), a.at(1).toDouble(), a.at(2).toDouble()};
    };
    v.shape = {shape[0].toInt(), shape[1].toInt(), shape[2].toInt()};
    v.voxel = triple("voxel");
    v.origin = triple("origin");
    v.bed = triple("bed");
    v.layersPerVoxel = meta.value("layersPerVoxel").toInt(1);
    const std::size_t n = static_cast<std::size_t>(v.shape[0]) * static_cast<std::size_t>(v.shape[1]) * static_cast<std::size_t>(v.shape[2]);
    const QByteArray bits = payload.mid(4 + metaSize);
    if (static_cast<std::size_t>(bits.size()) * 8 < n) return {};
    v.cells.resize(n);
    for (std::size_t i = 0; i < n; ++i)
        v.cells[i] = (static_cast<quint8>(bits[static_cast<qsizetype>(i / 8)]) >> (7 - i % 8)) & 1;
    return v;
}

QStringList slicedExtensions() { return {".cxdlpv4", ".cxdlp"}; }

bool isSlicedFile(const QString &path) {
    return slicedExtensions().contains("." + QFileInfo(path).suffix().toLower());
}

std::unique_ptr<SlicedFile> loadSliced(const QString &path) {
    const QString ext = "." + QFileInfo(path).suffix().toLower();
    if (ext == ".cxdlpv4" || ext == ".cxdlp") return std::make_unique<cxdlp::File>(path);
    throw FormatError(("unsupported file type: " + ext).toStdString());
}

}  // namespace resindeck
