#include "mesh/mesh.h"

#include <QtConcurrent/QtConcurrentMap>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <unordered_map>

namespace resindeck {

namespace {

constexpr quint32 kNone = std::numeric_limits<quint32>::max();
constexpr int kIso = 127;  // a sample is inside the model above this gray level

// Area-weighted vertex normals from the triangles.
void computeNormals(Mesh &m) {
    std::vector<float> &v = m.vertices;
    for (std::size_t i = 0; i < v.size(); i += 6) v[i + 3] = v[i + 4] = v[i + 5] = 0;
    for (std::size_t t = 0; t + 2 < m.indices.size(); t += 3) {
        const std::size_t a = m.indices[t] * 6ull, b = m.indices[t + 1] * 6ull, c = m.indices[t + 2] * 6ull;
        const float ux = v[b] - v[a], uy = v[b + 1] - v[a + 1], uz = v[b + 2] - v[a + 2];
        const float wx = v[c] - v[a], wy = v[c + 1] - v[a + 1], wz = v[c + 2] - v[a + 2];
        const float nx = uy * wz - uz * wy, ny = uz * wx - ux * wz, nz = ux * wy - uy * wx;
        for (std::size_t k : {a, b, c}) v[k + 3] += nx, v[k + 4] += ny, v[k + 5] += nz;
    }
    for (std::size_t i = 0; i < v.size(); i += 6) {
        const float len = std::sqrt(v[i + 3] * v[i + 3] + v[i + 4] * v[i + 4] + v[i + 5] * v[i + 5]);
        if (len > 0) v[i + 3] /= len, v[i + 4] /= len, v[i + 5] /= len;
    }
}

}  // namespace

// --- voxel mesh --------------------------------------------------------------------------

Mesh voxelMesh(const Voxels &v) {
    Mesh m;
    m.bed = v.bed;
    if (v.empty()) return m;
    const int nz = v.shape[0], ny = v.shape[1], nx = v.shape[2];
    auto solid = [&](int z, int y, int x) { return z >= 0 && y >= 0 && x >= 0 && z < nz && y < ny && x < nx && v.at(z, y, x); };
    // the six faces of a unit cube: neighbour offset, normal, four corners
    struct Face {
        int dz, dy, dx;
        float n[3];
        float c[4][3];
    };
    static const Face faces[6] = {
        {0, 0, 1, {1, 0, 0}, {{1, 0, 0}, {1, 1, 0}, {1, 1, 1}, {1, 0, 1}}},
        {0, 0, -1, {-1, 0, 0}, {{0, 0, 0}, {0, 0, 1}, {0, 1, 1}, {0, 1, 0}}},
        {0, 1, 0, {0, 1, 0}, {{0, 1, 0}, {0, 1, 1}, {1, 1, 1}, {1, 1, 0}}},
        {0, -1, 0, {0, -1, 0}, {{0, 0, 0}, {1, 0, 0}, {1, 0, 1}, {0, 0, 1}}},
        {1, 0, 0, {0, 0, 1}, {{0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}}},
        {-1, 0, 0, {0, 0, -1}, {{0, 0, 0}, {0, 1, 0}, {1, 1, 0}, {1, 0, 0}}},
    };
    const auto [vx, vy, vz] = v.voxel;
    const auto [ox, oy, oz] = v.origin;
    for (int z = 0; z < nz; ++z)
        for (int y = 0; y < ny; ++y)
            for (int x = 0; x < nx; ++x) {
                if (!v.at(z, y, x)) continue;
                for (const Face &f : faces) {
                    if (solid(z + f.dz, y + f.dy, x + f.dx)) continue;
                    const auto base = static_cast<quint32>(m.vertices.size() / 6);
                    for (const auto &c : f.c)
                        // image rows grow towards the front of the plate: flip y so the view matches
                        // the printer seen from the front
                        m.vertices.insert(m.vertices.end(), {static_cast<float>(ox + (double(x) + c[0]) * vx),
                                                             static_cast<float>(v.bed[1] - (oy + (double(y) + c[1]) * vy)),
                                                             static_cast<float>(oz + (double(z) + c[2]) * vz), f.n[0], -f.n[1], f.n[2]});
                    m.indices.insert(m.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
                }
            }
    m.top = oz + nz * vz;
    return m;
}

// --- smooth surface ----------------------------------------------------------------------

void modelBounds(const SlicedFile &file, const Voxels &v, SurfaceOptions &o) {
    if (v.empty()) return;
    const auto [px, py] = file.pixelMm();
    const int sx = std::max(1, int(std::lround(v.voxel[0] / px)));
    const int sy = std::max(1, int(std::lround(v.voxel[1] / py)));
    const int k = std::max(1, v.layersPerVoxel);
    const int x0 = int(std::lround(v.origin[0] / px)) - sx, y0 = int(std::lround(v.origin[1] / py)) - sy;
    const int l0 = int(std::lround(v.origin[2] / std::max(file.layerHeight(), 1e-6))) - k;
    o.pixels = {x0, y0, x0 + (v.shape[2] + 2) * sx, y0 + (v.shape[1] + 2) * sy};
    o.layers = {l0, l0 + (v.shape[0] + 2) * k};
}

namespace {
Mesh extractSurface(const SlicedFile &file, const SurfaceOptions &o);
}

Mesh surfaceMesh(const SlicedFile &file, const SurfaceOptions &options) {
    SurfaceOptions o = options;
    Mesh m = extractSurface(file, o);
    // too many triangles: the count grows with the surface (1/step²), the samples with the volume
    // (1/step³), so shrink the sample budget by the overshoot to the power 3/2
    for (int retry = 0; retry < 3 && m.indices.size() / 3 > o.maxTriangles; ++retry) {
        const double over = double(m.indices.size() / 3) / double(o.maxTriangles);
        o.sampleBudget = std::max<qint64>(1000, qint64(double(m.samples) / std::pow(over, 1.5) * 0.9));
        m = extractSurface(file, o);
    }
    return m;
}

namespace {

Mesh extractSurface(const SlicedFile &file, const SurfaceOptions &o) {
    Mesh m;
    m.bed = file.bed();
    const int W = file.width(), H = file.height(), L = file.layerCount();
    const bool whole = o.pixels == std::array<int, 4>{} && o.layers == std::array<int, 2>{};
    const int x0 = whole ? 0 : std::clamp(o.pixels[0], 0, W), x1 = whole ? W : std::clamp(o.pixels[2], 0, W);
    const int y0 = whole ? 0 : std::clamp(o.pixels[1], 0, H), y1 = whole ? H : std::clamp(o.pixels[3], 0, H);
    const int l0 = whole ? 0 : std::clamp(o.layers[0], 0, L), l1 = whole ? L : std::clamp(o.layers[1], 0, L);
    if (x1 <= x0 || y1 <= y0 || l1 <= l0) return m;

    // pixels per sample (s) and layers per sample (kz): the finest that fits the budget, keeping the
    // sample spacing about the same in every direction
    const auto [pxmm, pymm] = file.pixelMm();
    const double lh = std::max(file.layerHeight(), 1e-6);
    int s = 1, kz = 1;
    auto samples = [&] { return qint64((x1 - x0 + s - 1) / s + 2) * ((y1 - y0 + s - 1) / s + 2) * ((l1 - l0 + kz - 1) / kz + 2); };
    while (samples() > o.sampleBudget) {
        if (s * pxmm <= kz * lh) ++s;
        else ++kz;
    }
    m.step = s;
    m.layerStep = kz;
    m.samples = samples();

    // the sample grid has a zero margin all around, so the surface is closed
    const int nx = (x1 - x0 + s - 1) / s + 2, ny = (y1 - y0 + s - 1) / s + 2;
    const int nzLayers = (l1 - l0 + kz - 1) / kz, nz = nzLayers + 2;
    const std::size_t sliceSize = std::size_t(nx) * std::size_t(ny);

    // one slice: the layer averaged over s x s pixel blocks (keeps the anti-aliasing's information)
    auto slice = [&](int j) {
        std::vector<quint8> out(sliceSize, 0);
        if (j <= 0 || j > nzLayers) return out;  // zero padding below and above
        const QImage img = file.layer(l0 + (j - 1) * kz);
        if (img.isNull()) return out;
        const int area = s * s;
        for (int iy = 1; iy < ny - 1; ++iy) {
            const int py0 = y0 + (iy - 1) * s, py1 = std::min(py0 + s, y1);
            for (int ix = 1; ix < nx - 1; ++ix) {
                const int px0 = x0 + (ix - 1) * s, px1 = std::min(px0 + s, x1);
                int sum = 0;
                for (int py = py0; py < py1; ++py) {
                    const uchar *row = img.constScanLine(py);
                    for (int px = px0; px < px1; ++px) sum += row[px];
                }
                out[std::size_t(iy) * std::size_t(nx) + std::size_t(ix)] = static_cast<quint8>(sum / area);
            }
        }
        return out;
    };

    // sample grid -> mm on the plate (y flipped: image rows grow towards the front)
    auto toMm = [&](float sx, float sy, float sz, float *out) {
        out[0] = float((x0 + (double(sx) - 1) * s + s / 2.0) * pxmm);
        out[1] = float(m.bed[1] - (y0 + (double(sy) - 1) * s + s / 2.0) * pymm);
        out[2] = float((l0 + (double(sz) - 1) * kz + kz / 2.0) * lh);
    };

    const int cx = nx - 1, cy = ny - 1;  // cells per cell layer
    std::vector<quint32> prevCells(std::size_t(cx) * std::size_t(cy), kNone), cells(prevCells.size(), kNone);
    std::vector<quint8> lo = slice(0), hi;
    auto cellAt = [&](const std::vector<quint32> &layer, int x, int y) { return layer[std::size_t(y) * std::size_t(cx) + std::size_t(x)]; };
    // a quad around a grid edge, from four cells in counter-clockwise order seen from the edge's
    // positive end; flipped so it faces from inside to outside
    auto quad = [&](quint32 a, quint32 b, quint32 c, quint32 d, bool lowerInside) {
        if (a == kNone || b == kNone || c == kNone || d == kNone) return;
        // the y flip mirrors the mesh, which reverses the winding once more
        if (lowerInside) m.indices.insert(m.indices.end(), {a, c, b, a, d, c});
        else m.indices.insert(m.indices.end(), {a, b, c, a, c, d});
    };

    constexpr int kBatch = 32;
    std::vector<std::vector<quint8>> batch;
    int batchStart = 1;
    for (int c = 0; c < nz - 1; ++c) {  // cell layer c lies between slices c and c+1
        if (o.cancel && o.cancel->load()) return {};
        const int j = c + 1;
        if (j >= batchStart + int(batch.size())) {  // decode the next layers in parallel
            batchStart = j;
            QList<int> next;
            for (int k = j; k < std::min(j + kBatch, nz); ++k) next << k;
            batch = QtConcurrent::blockingMapped<std::vector<std::vector<quint8>>>(next, slice);
        }
        hi = std::move(batch[std::size_t(j - batchStart)]);
        std::fill(cells.begin(), cells.end(), kNone);

        // one vertex per cell the surface crosses, at the average of its edge crossings
        for (int y = 0; y < cy; ++y)
            for (int x = 0; x < cx; ++x) {
                const std::size_t i00 = std::size_t(y) * std::size_t(nx) + std::size_t(x), i10 = i00 + 1, i01 = i00 + std::size_t(nx), i11 = i01 + 1;
                const int v[8] = {lo[i00], lo[i10], lo[i01], lo[i11], hi[i00], hi[i10], hi[i01], hi[i11]};
                int inside = 0;
                for (int k = 0; k < 8; ++k) inside += v[k] > kIso;
                if (inside == 0 || inside == 8) continue;
                static constexpr int edges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
                float sx = 0, sy = 0, sz = 0;
                int n = 0;
                for (const auto &e : edges) {
                    const int a = v[e[0]], b = v[e[1]];
                    if ((a > kIso) == (b > kIso)) continue;
                    const float t = (float(kIso) + 0.5f - float(a)) / float(b - a);
                    // corner k: x = k & 1, y = (k >> 1) & 1, z = k >> 2
                    sx += float(e[0] & 1) + t * float((e[1] & 1) - (e[0] & 1));
                    sy += float((e[0] >> 1) & 1) + t * float(((e[1] >> 1) & 1) - ((e[0] >> 1) & 1));
                    sz += float(e[0] >> 2) + t * float((e[1] >> 2) - (e[0] >> 2));
                    ++n;
                }
                float pos[3];
                toMm(float(x) + sx / float(n), float(y) + sy / float(n), float(c) + sz / float(n), pos);
                cells[std::size_t(y) * std::size_t(cx) + std::size_t(x)] = static_cast<quint32>(m.vertices.size() / 6);
                m.vertices.insert(m.vertices.end(), {pos[0], pos[1], pos[2], 0, 0, 0});
                m.top = std::max(m.top, double(pos[2]));
            }

        // quads across the grid edges where inside meets outside
        for (int y = 0; y < ny - 1; ++y)
            for (int x = 0; x < nx - 1; ++x) {
                const std::size_t i = std::size_t(y) * std::size_t(nx) + std::size_t(x);
                const bool in = lo[i] > kIso;
                const bool innerX = x >= 1, innerY = y >= 1;  // the margin row/column has no cells before it
                // z edge from slice c to c+1: the four cells of this layer around it
                if (innerX && innerY && in != (hi[i] > kIso))
                    quad(cellAt(cells, x - 1, y - 1), cellAt(cells, x, y - 1), cellAt(cells, x, y), cellAt(cells, x - 1, y), in);
                if (c == 0) continue;  // slice 0 is padding: nothing on it
                // x edge on slice c: cells below (layer c-1) and above (layer c)
                if (innerY && in != (lo[i + 1] > kIso))
                    quad(cellAt(prevCells, x, y - 1), cellAt(prevCells, x, y), cellAt(cells, x, y), cellAt(cells, x, y - 1), in);
                // y edge on slice c
                if (innerX && in != (lo[i + std::size_t(nx)] > kIso))
                    quad(cellAt(prevCells, x - 1, y), cellAt(cells, x - 1, y), cellAt(cells, x, y), cellAt(prevCells, x, y), in);
            }

        std::swap(prevCells, cells);
        lo = std::move(hi);
        if (o.progress && c % 8 == 0) o.progress(double(c) / (nz - 1));
    }
    computeNormals(m);
    return m;
}

}  // namespace

// --- as printed --------------------------------------------------------------------------

Mesh printedMesh(const SlicedFile &file, const SurfaceOptions &o) {
    Mesh m;
    m.bed = file.bed();
    const int W = file.width(), H = file.height(), L = file.layerCount();
    const bool whole = o.pixels == std::array<int, 4>{} && o.layers == std::array<int, 2>{};
    const int x0 = whole ? 0 : std::clamp(o.pixels[0], 0, W), x1 = whole ? W : std::clamp(o.pixels[2], 0, W);
    const int y0 = whole ? 0 : std::clamp(o.pixels[1], 0, H), y1 = whole ? H : std::clamp(o.pixels[3], 0, H);
    const int l0 = whole ? 0 : std::clamp(o.layers[0], 0, L), l1 = whole ? L : std::clamp(o.layers[1], 0, L);
    if (x1 <= x0 || y1 <= y0 || l1 <= l0) return m;
    const int bw = x1 - x0, bh = y1 - y0;
    const auto [pxmm, pymm] = file.pixelMm();
    const double lh = std::max(file.layerHeight(), 1e-6);
    m.step = m.layerStep = 1;

    // grid corner -> mm on the plate (y flipped: image rows grow towards the front)
    auto X = [&](int px) { return float((x0 + px) * pxmm); };
    auto Y = [&](int py) { return float(m.bed[1] - (y0 + py) * pymm); };
    auto Z = [&](int layer) { return float(layer * lh); };
    // a rectangle from corner p along u and v, wound counter-clockwise seen from where n points
    auto rect = [&](const std::array<float, 3> &p, const std::array<float, 3> &u, const std::array<float, 3> &v,
                    const std::array<float, 3> &n) {
        const float cx = u[1] * v[2] - u[2] * v[1], cy = u[2] * v[0] - u[0] * v[2], cz = u[0] * v[1] - u[1] * v[0];
        const bool flip = cx * n[0] + cy * n[1] + cz * n[2] < 0;
        const auto &a = flip ? v : u, &b = flip ? u : v;
        const auto base = static_cast<quint32>(m.vertices.size() / 6);
        for (const auto &c : {std::array<float, 3>{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}})
            m.vertices.insert(m.vertices.end(), {p[0] + c[0] * a[0] + c[1] * b[0], p[1] + c[0] * a[1] + c[1] * b[1],
                                                 p[2] + c[0] * a[2] + c[1] * b[2], n[0], n[1], n[2]});
        m.indices.insert(m.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    };
    auto full = [&] { return m.indices.size() / 3 > o.maxTriangles; };

    // walls are merged along their edge within a layer, then across layers while they stay the same:
    // key = kind (x or y wall) | facing | position | from | to, value = first layer
    auto wallKey = [](quint64 kind, quint64 facing, quint64 at, quint64 from, quint64 to) {
        return (kind << 63) | (facing << 62) | (at << 42) | (from << 21) | to;
    };
    std::unordered_map<quint64, int> open, next;
    auto emitWall = [&](quint64 key, int z0, int z1) {
        const bool yWall = key >> 63, facing = (key >> 62) & 1;
        const int at = int((key >> 42) & 0xFFFFF), from = int((key >> 21) & 0x1FFFFF), to = int(key & 0x1FFFFF);
        const float zb = Z(z0), dz = Z(z1) - zb;
        if (!yWall)  // between columns at-1 and at, rows from..to; facing: solid on the left, faces +x
            rect({X(at), Y(from), zb}, {0, Y(to) - Y(from), 0}, {0, 0, dz}, {facing ? 1.f : -1.f, 0, 0});
        else  // between rows at-1 and at, columns from..to; facing: solid above (row at-1), faces +row = -y
            rect({X(from), Y(at), zb}, {X(to) - X(from), 0, 0}, {0, 0, dz}, {0, facing ? -1.f : 1.f, 0});
    };

    // flat faces between two layers, merged into rectangles: runs of the same kind in each row,
    // extended down the rows while they repeat; kind 1 = top of the layer below, 2 = bottom of the one above
    struct Run {
        int x0, x1, kind, y0;
    };
    auto caps = [&](const std::vector<quint8> &below, const std::vector<quint8> &above, int layer) {
        std::vector<Run> prev, cur;
        auto closeRun = [&](const Run &r, int yEnd) {
            rect({X(r.x0), Y(r.y0), Z(layer)}, {X(r.x1) - X(r.x0), 0, 0}, {0, Y(yEnd) - Y(r.y0), 0},
                 {0, 0, r.kind == 1 ? 1.f : -1.f});
        };
        for (int y = 0; y <= bh; ++y) {
            cur.clear();
            const quint8 *b = below.data() + std::size_t(y) * std::size_t(bw), *a = above.data() + std::size_t(y) * std::size_t(bw);
            if (y < bh && std::memcmp(a, b, std::size_t(bw)) != 0) {
                for (int x = 0; x < bw;) {
                    const int kind = b[x] == a[x] ? 0 : b[x] ? 1 : 2;
                    int e = x + 1;
                    while (e < bw && (b[e] == a[e] ? 0 : b[e] ? 1 : 2) == kind) ++e;
                    if (kind) cur.push_back({x, e, kind, y});
                    x = e;
                }
            }
            std::size_t j = 0;  // both lists are sorted by x0: carry on the runs that repeat
            for (Run &r : cur) {
                while (j < prev.size() && prev[j].x0 < r.x0) closeRun(prev[j], y), ++j;
                if (j < prev.size() && prev[j].x0 == r.x0 && prev[j].x1 == r.x1 && prev[j].kind == r.kind) r.y0 = prev[j++].y0;
            }
            for (; j < prev.size(); ++j) closeRun(prev[j], y);
            std::swap(prev, cur);
        }
    };

    // per layer, in parallel: solid (1) / empty (0) pixels over the model's box, and its walls merged
    // along their edge (keys as above)
    struct Layer {
        std::vector<quint8> mask;
        std::vector<quint64> walls;
    };
    auto readLayer = [&](int layer) {
        Layer out;
        out.mask.assign(std::size_t(bw) * std::size_t(bh), 0);
        const QImage img = layer >= l0 && layer < l1 ? file.layer(layer) : QImage();
        if (img.isNull()) return out;
        std::vector<char> any(std::size_t(bh) + 1, 0);
        for (int y = 0; y < bh; ++y) {
            const uchar *row = img.constScanLine(y0 + y) + x0;
            quint8 *dst = out.mask.data() + std::size_t(y) * std::size_t(bw);
            char a = 0;
            for (int x = 0; x < bw; ++x) a |= char(dst[x] = row[x] > kIso);
            any[std::size_t(y)] = a;
        }
        auto row = [&](int y) { return out.mask.data() + std::size_t(y) * std::size_t(bw); };
        // x walls (between columns), found row by row and carried down the rows while they repeat
        struct Edge {
            int x;
            bool facing;
            int y0;
        };
        std::vector<Edge> prev, cur;
        for (int y = 0; y <= bh; ++y) {
            cur.clear();
            if (y < bh && any[std::size_t(y)]) {
                const quint8 *r = row(y);
                for (int x = 0; x <= bw; ++x) {
                    const bool l = x > 0 && r[x - 1], here = x < bw && r[x];
                    if (l != here) cur.push_back({x, l, y});
                }
            }
            std::size_t j = 0;
            for (Edge &e : cur) {
                while (j < prev.size() && prev[j].x < e.x) {
                    out.walls.push_back(wallKey(0, prev[j].facing, quint64(prev[j].x), quint64(prev[j].y0), quint64(y)));
                    ++j;
                }
                if (j < prev.size() && prev[j].x == e.x && prev[j].facing == e.facing) e.y0 = prev[j++].y0;
            }
            for (; j < prev.size(); ++j)
                out.walls.push_back(wallKey(0, prev[j].facing, quint64(prev[j].x), quint64(prev[j].y0), quint64(y)));
            std::swap(prev, cur);
        }
        // y walls (between rows), in runs along the row
        for (int y = 0; y <= bh; ++y) {
            const bool hasUp = y > 0 && any[std::size_t(y - 1)], hasDown = y < bh && any[std::size_t(y)];
            if (!hasUp && !hasDown) continue;
            const quint8 *up = hasUp ? row(y - 1) : nullptr, *down = hasDown ? row(y) : nullptr;
            if (up && down && std::memcmp(up, down, std::size_t(bw)) == 0) continue;
            for (int x = 0; x < bw;) {
                const bool u = up && up[x], d = down && down[x];
                if (u == d) {
                    ++x;
                    continue;
                }
                int e = x + 1;
                while (e < bw && bool(up && up[e]) == u && bool(down && down[e]) == d) ++e;
                out.walls.push_back(wallKey(1, u, quint64(y), quint64(x), quint64(e)));
                x = e;
            }
        }
        return out;
    };

    constexpr int kBatch = 32;
    std::vector<Layer> batch;
    int batchStart = l0;
    std::vector<quint8> below(std::size_t(bw) * std::size_t(bh), 0);
    Layer cur;
    for (int k = l0; k <= l1; ++k) {
        if (o.cancel && o.cancel->load()) return {};
        if (k < l1) {
            if (k >= batchStart + int(batch.size())) {  // read the next layers in parallel
                batchStart = k;
                QList<int> list;
                for (int n = k; n < std::min(k + kBatch, l1); ++n) list << n;
                batch = QtConcurrent::blockingMapped<std::vector<Layer>>(list, readLayer);
            }
            cur = std::move(batch[std::size_t(k - batchStart)]);
        } else {
            cur = {std::vector<quint8>(below.size(), 0), {}};  // above the last layer
        }
        caps(below, cur.mask, k);
        if (full()) break;

        // walls: those that continue from the layer below keep their first layer
        next.clear();
        for (quint64 key : cur.walls) {
            const auto it = open.find(key);
            next.emplace(key, it != open.end() ? it->second : k);
        }
        for (const auto &[key, z0] : open)
            if (!next.count(key)) emitWall(key, z0, k);
        std::swap(open, next);
        if (full()) break;
        below = std::move(cur.mask);
        if (o.progress && (k - l0) % 8 == 0) o.progress(double(k - l0) / (l1 - l0 + 1));
    }
    if (full()) {
        Mesh large;
        large.bed = m.bed;
        large.tooLarge = true;
        return large;
    }
    for (std::size_t i = 2; i < m.vertices.size(); i += 6) m.top = std::max(m.top, double(m.vertices[i]));
    return m;
}

}  // namespace resindeck
