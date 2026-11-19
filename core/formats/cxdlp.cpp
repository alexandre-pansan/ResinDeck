#include "formats/cxdlp.h"

#include <QColor>
#include <QFileInfo>
#include <QJsonArray>
#include <QSaveFile>
#include <QtEndian>

#include <cmath>
#include <cstring>

namespace resindeck::cxdlp {

namespace {

constexpr qint64 kHeaderTailSize = 80;  // <HHfffffIIIIIIIIIIHHIII
constexpr qint64 kPreviewSize = 32;     // <8I
constexpr qint64 kParamsSize = 68;      // <10fI2f4I
constexpr qint64 kLayerDefSize = 40;    // <3f7I
constexpr qint64 kLayerDefExSize = 44;  // per-layer header before the encoded image

// Little-endian reader over a buffer; reads past the end give zeros.
class Reader {
public:
    explicit Reader(const QByteArray &data) : data_(data) {}
    template <typename T>
    T get() {
        T v{};
        if (pos_ + qsizetype(sizeof(T)) <= data_.size()) v = qFromLittleEndian<T>(data_.constData() + pos_);
        pos_ += sizeof(T);
        return v;
    }
    double f32() { return static_cast<double>(get<float>()); }
    quint32 u32() { return get<quint32>(); }
    quint16 u16() { return get<quint16>(); }

private:
    const QByteArray &data_;
    qsizetype pos_ = 0;
};

class Buffer {
public:
    QByteArray data;
    template <typename T>
    void put(T v) {
        char raw[sizeof(T)];
        qToLittleEndian(v, raw);
        data.append(raw, sizeof(T));
    }
    void f32(double v) { put(static_cast<float>(v)); }
    void u32(qint64 v) { put(static_cast<quint32>(v)); }
    void u16(int v) { put(static_cast<quint16>(v)); }
    void be32(qint64 v) {
        char raw[4];
        qToBigEndian(static_cast<quint32>(v), raw);
        data.append(raw, 4);
    }
};

QString cString(const QByteArray &b) {
    const qsizetype nul = b.indexOf('\0');
    return QString::fromLatin1(nul < 0 ? b : b.left(nul));
}

double rounded(double v, int digits) {
    const double f = std::pow(10.0, digits);
    return std::round(v * f) / f;
}

uchar gray3(int g) { return static_cast<uchar>((g << 5) | (g << 2) | (g >> 1)); }

QImage grayImage(const std::vector<uchar> &px, int width, int height) {
    QImage img(width, height, QImage::Format_Grayscale8);
    for (int y = 0; y < height; ++y)
        std::memcpy(img.scanLine(y), px.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(width),
                    static_cast<std::size_t>(width));
    return img;
}

}  // namespace

// --- reading -----------------------------------------------------------------------------

File::File(const QString &path) : path_(path), file_(path) {
    if (!file_.open(QIODevice::ReadOnly))
        throw FormatError(("cannot open " + path + ": " + file_.errorString()).toStdString());

    auto be32 = [this] {
        const QByteArray b = file_.read(4);
        return b.size() == 4 ? qFromBigEndian<quint32>(b.constData()) : 0u;
    };
    const quint32 magicSize = be32();
    const QString magic = cString(file_.read(std::min<quint32>(magicSize, 64)));
    if (!magic.startsWith("CXSW3D")) throw FormatError(("not a CXDLPv4 file (" + magic + ")").toStdString());
    file_.read(2);  // version, big-endian
    const quint32 nameSize = be32();
    printer_ = cString(file_.read(std::min<quint32>(nameSize, 256)));

    const QByteArray tail = file_.read(kHeaderTailSize);
    if (tail.size() != kHeaderTailSize) throw FormatError("truncated CXDLPv4 header");
    Reader r(tail);
    resX_ = r.u16();
    resY_ = r.u16();
    bed_ = {r.f32(), r.f32(), r.f32()};
    printHeight_ = r.f32();
    layerHeight_ = r.f32();
    bottomLayers_ = static_cast<int>(r.u32());
    const quint32 previewSmall = r.u32();
    const quint32 layersOffset = r.u32();
    const quint32 layerCount = r.u32();
    const quint32 previewLarge = r.u32();
    printTime_ = static_cast<int>(r.u32());
    r.u32();  // projector type
    const quint32 paramsOffset = r.u32();
    r.u32();  // params size
    antiAlias_ = static_cast<int>(r.u32());
    r.u16();  // light PWM
    r.u16();  // bottom light PWM
    encryptionKey_ = r.u32();
    if (resX_ <= 0 || resY_ <= 0) throw FormatError("invalid resolution in CXDLPv4 header");

    for (quint32 off : {previewSmall, previewLarge}) {
        if (!off) continue;
        const QByteArray b = readAt(off, kPreviewSize);
        Reader p(b);
        Preview pv;
        pv.width = static_cast<int>(p.u32());
        pv.height = static_cast<int>(p.u32());
        pv.offset = p.u32();
        pv.length = p.u32();
        previews_.push_back(pv);
    }

    if (paramsOffset) {
        const QByteArray b = readAt(paramsOffset, kParamsSize);
        Reader p(b);
        Params &q = params_;
        q.bottomLiftHeight = p.f32(), q.bottomLiftSpeed = p.f32(), q.liftHeight = p.f32(), q.liftSpeed = p.f32();
        q.retractSpeed = p.f32(), q.volumeMl = p.f32(), q.weightG = p.f32(), q.cost = p.f32();
        q.bottomLightOff = p.f32(), q.lightOff = p.f32();
        p.u32();
        q.exposure = p.f32(), q.bottomExposure = p.f32();
        hasParams_ = true;
    }

    if (layerCount > 1'000'000) throw FormatError("implausible layer count in CXDLPv4 header");
    const QByteArray table = readAt(layersOffset, kLayerDefSize * layerCount);
    if (table.size() != kLayerDefSize * layerCount) throw FormatError("truncated CXDLPv4 layer table");
    for (quint32 i = 0; i < layerCount; ++i) {
        const QByteArray entry = table.mid(static_cast<qsizetype>(i) * kLayerDefSize, kLayerDefSize);
        Reader l(entry);
        Layer layer;
        layer.z = l.f32();
        layer.exposure = l.f32();
        layer.lightOff = l.f32();
        layer.address = l.u32();
        layer.size = l.u32();
        layer.dataType = l.u32();
        layers_.push_back(layer);
    }

    if (!layers_.empty() && !encryptionKey_) pixelPair_ = isPixelPair(layerData(0), resX_, resY_);
}

QByteArray File::readAt(qint64 offset, qint64 length) const {
    QMutexLocker lock(&fileMutex_);
    if (!file_.seek(offset)) return {};
    return file_.read(length);
}

QJsonObject File::info() const {
    QJsonObject o{
        {"file", QFileInfo(path_).fileName()},
        {"size", QFileInfo(path_).size()},
        {"printer", printer_},
        {"resolution", QJsonArray{resX_, resY_}},
        {"bed", QJsonArray{rounded(bed_[0], 2), rounded(bed_[1], 2), rounded(bed_[2], 2)}},
        {"printHeight", rounded(printHeight_, 3)},
        {"layerHeight", rounded(layerHeight_, 4)},
        {"layerCount", layerCount()},
        {"bottomLayers", bottomLayers_},
        {"printTime", printTime_},
        {"antiAlias", antiAlias_},
        {"previews", previewCount()},
    };
    if (hasParams_) {
        const Params &q = params_;
        const std::pair<const char *, double> values[] = {
            {"bottomLiftHeight", q.bottomLiftHeight}, {"bottomLiftSpeed", q.bottomLiftSpeed},
            {"liftHeight", q.liftHeight},             {"liftSpeed", q.liftSpeed},
            {"retractSpeed", q.retractSpeed},         {"volumeMl", q.volumeMl},
            {"weightG", q.weightG},                   {"cost", q.cost},
            {"bottomLightOff", q.bottomLightOff},     {"lightOff", q.lightOff},
            {"exposure", q.exposure},                 {"bottomExposure", q.bottomExposure},
        };
        for (const auto &[key, value] : values) o.insert(key, rounded(value, 3));
    }
    return o;
}

QByteArray File::layerData(int index) const {
    const Layer &l = layers_.at(static_cast<std::size_t>(index));
    if (encryptionKey_) throw FormatError("encrypted layers are not supported");
    if (l.size < kLayerDefExSize) return {};
    return readAt(l.address + kLayerDefExSize, l.size - kLayerDefExSize);
}

QImage File::layer(int index) const {
    const QByteArray rle = layerData(index);
    return pixelPair_ ? decodePixelPair(rle, resX_, resY_) : decodeRle7(rle, resX_, resY_);
}

QImage File::preview(int index) const {
    if (previews_.empty()) return {};
    const int n = previewCount();
    const Preview &p = previews_.at(static_cast<std::size_t>(index < 0 ? n + index : index));
    return decodeRgb15(readAt(p.offset, p.length), p.width, p.height);
}

// --- encodings ---------------------------------------------------------------------------

// 7-bit run-length encoding: a byte with the top bit clear is one pixel of gray (code << 1 | 1, or
// black for code 0); with the top bit set it is followed by a run length of 1 to 4 bytes.
QImage decodeRle7(const QByteArray &rle, int width, int height) {
    const auto n = static_cast<qsizetype>(width) * height;
    std::vector<uchar> out(static_cast<std::size_t>(n), 0);
    const auto *b = reinterpret_cast<const uchar *>(rle.constData());
    const qsizetype len = rle.size();
    qsizetype pos = 0, i = 0;
    auto at = [&](qsizetype k) -> uint { return k < len ? b[k] : 0u; };
    while (i < len) {
        uint code = b[i++];
        qsizetype stride = 1;
        if (code & 0x80) {
            code &= 0x7F;
            const uint s = at(i);
            if (!(s & 0x80)) {
                stride = s;
                i += 1;
            } else if ((s & 0xC0) == 0x80) {
                stride = ((s & 0x3F) << 8) | at(i + 1);
                i += 2;
            } else if ((s & 0xE0) == 0xC0) {
                stride = ((s & 0x1F) << 16) | (at(i + 1) << 8) | at(i + 2);
                i += 3;
            } else {
                stride = static_cast<qsizetype>(((s & 0x0F) << 24) | (at(i + 1) << 16) | (at(i + 2) << 8) | at(i + 3));
                i += 4;
            }
        }
        if (code) {
            const qsizetype end = std::min(pos + stride, n);
            if (pos < end) std::memset(out.data() + pos, static_cast<int>((code << 1) | 1), static_cast<std::size_t>(end - pos));
        }
        pos += stride;
    }
    return grayImage(out, width, height);
}

// Two pixels per unit: code 0x80 | right << 3 | left (3-bit grays), then the repeat count in
// 7-bit groups.
QImage decodePixelPair(const QByteArray &rle, int width, int height) {
    const auto n = static_cast<qsizetype>(width) * height;
    std::vector<uchar> out(static_cast<std::size_t>(n), 0);
    const auto *b = reinterpret_cast<const uchar *>(rle.constData());
    const qsizetype len = rle.size();
    qsizetype unit = 0, i = 0;
    while (i < len) {
        const uint code = b[i++];
        qsizetype rep = 0;
        while (i < len && !(b[i] & 0x80)) rep = (rep << 7) | b[i++];
        const qsizetype count = rep + 1;
        const uchar left = gray3(code & 7), right = gray3((code >> 3) & 7);
        if (left || right)
            for (qsizetype u = unit; u < unit + count && u * 2 + 1 < n; ++u) {
                out[static_cast<std::size_t>(u * 2)] = left;
                out[static_cast<std::size_t>(u * 2 + 1)] = right;
            }
        unit += count;
    }
    return grayImage(out, width, height);
}

bool isPixelPair(const QByteArray &rle, int width, int height) {
    if (width % 2) return false;
    const qsizetype perRow = width / 2, total = perRow * height;
    const auto *b = reinterpret_cast<const uchar *>(rle.constData());
    const qsizetype len = rle.size();
    qsizetype unit = 0, i = 0;
    while (i < len) {
        const uint code = b[i++];
        if (!(code & 0x80) || (code & 0x40) || unit >= total) return false;
        const qsizetype remaining = perRow - unit % perRow;
        qsizetype rep = 0;
        while (i < len && !(b[i] & 0x80)) {
            rep = (rep << 7) | b[i++];
            if (rep >= remaining) return false;
        }
        unit += rep + 1;
    }
    return unit == total;
}

QByteArray encodeRle7(const QImage &gray) {
    const QImage img = gray.convertToFormat(QImage::Format_Grayscale8);
    std::vector<uchar> flat;
    flat.reserve(static_cast<std::size_t>(img.width()) * static_cast<std::size_t>(img.height()));
    for (int y = 0; y < img.height(); ++y) {
        const uchar *row = img.constScanLine(y);
        for (int x = 0; x < img.width(); ++x) flat.push_back(static_cast<uchar>(row[x] >> 1));
    }
    QByteArray out;
    std::size_t i = 0;
    while (i < flat.size()) {
        const uchar code = flat[i];
        std::size_t j = i;
        while (j < flat.size() && flat[j] == code) ++j;
        const auto n = static_cast<quint32>(j - i);
        if (n == 1) {
            out.append(static_cast<char>(code));
        } else {
            out.append(static_cast<char>(code | 0x80));
            if (n < 0x80) {
                out.append(static_cast<char>(n));
            } else if (n < 0x4000) {
                out.append(static_cast<char>(0x80 | (n >> 8))).append(static_cast<char>(n & 0xFF));
            } else if (n < 0x200000) {
                out.append(static_cast<char>(0xC0 | (n >> 16))).append(static_cast<char>((n >> 8) & 0xFF)).append(static_cast<char>(n & 0xFF));
            } else {
                out.append(static_cast<char>(0xE0 | (n >> 24)))
                    .append(static_cast<char>((n >> 16) & 0xFF))
                    .append(static_cast<char>((n >> 8) & 0xFF))
                    .append(static_cast<char>(n & 0xFF));
            }
        }
        i = j;
    }
    return out;
}

// Chitu RGB15 previews: little-endian words R5 G5 B5 (bit 0x20 unused by color), where 0x20 marks
// a run whose length - 1 follows in the next word's low 12 bits.
QImage decodeRgb15(const QByteArray &data, int width, int height) {
    if (width <= 0 || height <= 0) return {};
    QImage img(width, height, QImage::Format_RGB888);
    img.fill(Qt::black);
    const qsizetype total = static_cast<qsizetype>(width) * height, words = data.size() / 2;
    qsizetype pos = 0, i = 0;
    auto word = [&](qsizetype k) { return qFromLittleEndian<quint16>(data.constData() + k * 2); };
    while (i < words && pos < total) {
        const quint16 dot = word(i++);
        qsizetype rep = 1;
        if ((dot & 0x20) && i < words) rep += word(i++) & 0xFFF;
        const auto r = static_cast<uchar>(((dot >> 11) & 0x1F) << 3), g = static_cast<uchar>(((dot >> 6) & 0x1F) << 3),
                   b = static_cast<uchar>((dot & 0x1F) << 3);
        for (qsizetype k = pos; k < std::min(pos + rep, total); ++k) {
            uchar *px = img.scanLine(static_cast<int>(k / width)) + (k % width) * 3;
            px[0] = r, px[1] = g, px[2] = b;
        }
        pos += rep;
    }
    return img;
}

QByteArray encodeRgb15(const QImage &rgb) {
    const QImage img = rgb.convertToFormat(QImage::Format_RGB888);
    std::vector<quint16> dots;
    for (int y = 0; y < img.height(); ++y) {
        const uchar *row = img.constScanLine(y);
        for (int x = 0; x < img.width(); ++x)
            dots.push_back(static_cast<quint16>(((row[x * 3] >> 3) << 11) | ((row[x * 3 + 1] >> 3) << 6) | (row[x * 3 + 2] >> 3)));
    }
    Buffer out;
    std::size_t i = 0;
    while (i < dots.size()) {
        std::size_t j = i;
        while (j < dots.size() && dots[j] == dots[i] && j - i < 0x1000) ++j;
        if (j - i == 1) {
            out.u16(dots[i]);
        } else {
            out.u16(dots[i] | 0x20);
            out.u16(static_cast<int>((j - i - 1) & 0xFFF));
        }
        i = j;
    }
    return out.data;
}

quint32 footerCrc(const QByteArray &data) {
    static const auto table = [] {
        std::array<quint32, 256> t{};
        for (quint32 n = 0; n < 256; ++n) {
            quint32 c = n;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[n] = c;
        }
        return t;
    }();
    quint32 crc = 0;
    for (char ch : data) crc = table[(crc ^ static_cast<uchar>(ch)) & 0xFF] ^ (crc >> 8);
    return crc;
}

bool verifyChecksum(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly) || f.size() < 4) return false;
    const QByteArray data = f.readAll();
    return footerCrc(data.left(data.size() - 4)) == qFromBigEndian<quint32>(data.constData() + data.size() - 4);
}

// --- writing -----------------------------------------------------------------------------

void Writer::write(const QString &path) const {
    QImage thumb = preview;
    if (thumb.isNull()) {  // red left half, blue right half
        thumb = QImage(16, 12, QImage::Format_RGB888);
        for (int y = 0; y < 12; ++y)
            for (int x = 0; x < 16; ++x) thumb.setPixelColor(x, y, x < 8 ? QColor(248, 0, 0) : QColor(0, 0, 248));
    }
    const int resX = layers.empty() ? 0 : layers.front().width();
    const int resY = layers.empty() ? 0 : layers.front().height();
    const QByteArray name = printer.toLatin1() + '\0';

    Buffer body;
    body.be32(magic.size());
    body.data += magic;
    body.data.append("\x00\x04", 2);  // version 4, big-endian
    body.be32(name.size());
    body.data += name;
    const qsizetype tailAt = body.data.size();
    body.data.append(QByteArray(kHeaderTailSize, '\0'));

    const QByteArray img = encodeRgb15(thumb);
    auto placePreview = [&] {
        const qsizetype off = body.data.size();
        Buffer p;
        p.u32(thumb.width()), p.u32(thumb.height()), p.u32(off + kPreviewSize), p.u32(img.size());
        for (int k = 0; k < 4; ++k) p.u32(0);
        body.data += p.data + img;
        return off;
    };
    const qsizetype previewSmall = placePreview();
    const qsizetype previewLarge = placePreview();

    const qsizetype paramsOffset = body.data.size();
    const Params &q = params;
    for (double v : {q.bottomLiftHeight, q.bottomLiftSpeed, q.liftHeight, q.liftSpeed, q.retractSpeed, q.volumeMl,
                     q.weightG, q.cost, q.bottomLightOff, q.lightOff})
        body.f32(v);
    body.u32(0);
    body.f32(q.exposure);
    body.f32(q.bottomExposure);
    for (int k = 0; k < 4; ++k) body.u32(0);

    const qsizetype tableOffset = body.data.size();
    body.data.append(QByteArray(kLayerDefSize * static_cast<qsizetype>(layers.size()), '\0'));
    Buffer table;
    for (std::size_t n = 0; n < layers.size(); ++n) {
        const QByteArray data = QByteArray(kLayerDefExSize, '\0') + encodeRle7(layers[n]);
        const qsizetype address = body.data.size();
        body.data += data;
        table.f32(static_cast<double>(n + 1) * layerHeight), table.f32(q.exposure), table.f32(q.lightOff);
        table.u32(address), table.u32(data.size());
        for (int k = 0; k < 5; ++k) table.u32(0);
    }
    body.data.replace(tableOffset, table.data.size(), table.data);

    Buffer tail;
    tail.u16(resX), tail.u16(resY);
    tail.f32(bed[0]), tail.f32(bed[1]), tail.f32(bed[2]);
    tail.f32(static_cast<double>(layers.size()) * layerHeight), tail.f32(layerHeight);
    tail.u32(bottomLayers), tail.u32(previewSmall), tail.u32(tableOffset), tail.u32(static_cast<qint64>(layers.size()));
    tail.u32(previewLarge), tail.u32(printTime), tail.u32(0), tail.u32(paramsOffset), tail.u32(kParamsSize);
    tail.u32(antiAlias), tail.u16(255), tail.u16(255);
    tail.u32(encrypted ? 1 : 0), tail.u32(0), tail.u32(0);
    body.data.replace(tailAt, kHeaderTailSize, tail.data);

    quint32 crc = footerCrc(body.data);
    if (corruptCrc) crc ^= 1;
    body.be32(crc);

    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly) || out.write(body.data) != body.data.size() || !out.commit())
        throw FormatError(("cannot write " + path + ": " + out.errorString()).toStdString());
}

}  // namespace resindeck::cxdlp
