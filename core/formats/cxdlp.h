// Creality .cxdlpv4 sliced files (magic "CXSW3DV2"), used by HALOT printers.
// Layout based on UVtools' CrealityCXDLPv4File.cs (AGPL; the format description only, no code).
#pragma once

#include "formats/sliced.h"

#include <QFile>
#include <QImage>
#include <QJsonObject>
#include <QList>
#include <QMutex>
#include <QString>

#include <array>
#include <vector>

namespace resindeck::cxdlp {

struct Layer {
    double z = 0;          // mm
    double exposure = 0;   // s
    double lightOff = 0;   // s
    quint32 address = 0;   // offset of the layer's data in the file
    quint32 size = 0;
    quint32 dataType = 0;
};

struct Preview {
    int width = 0, height = 0;
    quint32 offset = 0, length = 0;
};

// The print settings stored in the file.
struct Params {
    double bottomLiftHeight = 0, bottomLiftSpeed = 0, liftHeight = 0, liftSpeed = 0, retractSpeed = 0;
    double volumeMl = 0, weightG = 0, cost = 0, bottomLightOff = 0, lightOff = 0;
    double exposure = 0, bottomExposure = 0;
};

class File : public SlicedFile {
public:
    // Reads the header, previews and layer table. Throws FormatError.
    explicit File(const QString &path);

    QJsonObject info() const override;
    int width() const override { return resX_; }
    int height() const override { return resY_; }
    std::array<double, 3> bed() const override { return bed_; }
    double layerHeight() const override { return layerHeight_; }
    int layerCount() const override { return static_cast<int>(layers_.size()); }
    double layerZ(int index) const override { return layers_.at(static_cast<std::size_t>(index)).z; }
    int previewCount() const override { return static_cast<int>(previews_.size()); }
    QImage preview(int index = -1) const override;
    QImage layer(int index) const override;

    const QString &printer() const { return printer_; }
    const Params &params() const { return params_; }
    const std::vector<Layer> &layers() const { return layers_; }
    int printTime() const { return printTime_; }
    int antiAlias() const { return antiAlias_; }
    int bottomLayers() const { return bottomLayers_; }
    double printHeight() const { return printHeight_; }
    bool pixelPair() const { return pixelPair_; }
    // The layer's encoded bytes. Throws FormatError for encrypted files.
    QByteArray layerData(int index) const;

private:
    QByteArray readAt(qint64 offset, qint64 length) const;

    QString path_;
    mutable QMutex fileMutex_;  // layers are read from several threads at once
    mutable QFile file_;
    QString printer_;
    int resX_ = 0, resY_ = 0;
    std::array<double, 3> bed_{};
    double printHeight_ = 0, layerHeight_ = 0;
    int bottomLayers_ = 0, printTime_ = 0, antiAlias_ = 0;
    quint32 encryptionKey_ = 0;
    Params params_;
    bool hasParams_ = false;
    std::vector<Preview> previews_;
    std::vector<Layer> layers_;
    bool pixelPair_ = false;  // layers in the 2-pixels-per-byte encoding of newer slicers
};

// The file's trailing checksum matches its contents.
bool verifyChecksum(const QString &path);

// Layer encodings, exposed for tests.
QImage decodeRle7(const QByteArray &rle, int width, int height);
QImage decodePixelPair(const QByteArray &rle, int width, int height);
bool isPixelPair(const QByteArray &rle, int width, int height);
QByteArray encodeRle7(const QImage &gray);     // the inverse of decodeRle7
QByteArray encodeRgb15(const QImage &rgb);     // preview encoding
QImage decodeRgb15(const QByteArray &data, int width, int height);
// CRC-32 as the file footer uses it: reflected, polynomial 0xEDB88320, starting at 0, no final xor.
quint32 footerCrc(const QByteArray &data);

// Number of lit (not black) pixels in an encoded RLE7 layer.
qint64 rle7LitPixels(const QByteArray &rle);

// Writes .cxdlpv4 files laid out as HALOT BOX and Chitubox write them for HALOT printers: the header,
// a small (120x120) and a large (300x300) preview, the print settings, the layer table, then each layer
// with its lift settings and its RLE7 image, and the checksum.
struct Writer {
    QString printer = "HALOT-R6";
    QByteArray magic = QByteArray("CXSW3DV2\0", 9);
    std::array<double, 3> bed{6.0, 4.0, 50.0};
    double layerHeight = 0.05;
    int bottomLayers = 2;
    int printTime = 1234;  // s
    int antiAlias = 4;
    Params params{5.0, 60.0, 4.0, 90.0, 150.0, 1.25, 1.5, 0.1, 1.0, 0.5, 2.5, 30.0};
    int lightPwm = 255, bottomLightPwm = 255;
    QImage previewSmall, previewLarge;  // RGB; an empty one is scaled from the other (both empty: a 16x12 test image)
    QByteArray encodedPreviewSmall, encodedPreviewLarge;  // already encoded (RGB15) previews of those sizes
    // The layers, as images (gray, all the same size) or already encoded with encodeRle7; with both, the
    // encoded ones are used and the images only give the resolution.
    std::vector<QImage> layers;
    std::vector<QByteArray> encodedLayers;
    int width = 0, height = 0;  // resolution when only encodedLayers is set
    bool encrypted = false;     // set the encryption flag (the data stays plain); for tests
    bool corruptCrc = false;    // for tests

    // Throws FormatError if the file cannot be written.
    void write(const QString &path) const;
    // The file's bytes.
    QByteArray bytes() const;
};

}  // namespace resindeck::cxdlp
