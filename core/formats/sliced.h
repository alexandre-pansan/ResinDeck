// Sliced files (what a printer prints): header values, previews and layer images, whatever the
// format. A reader is a subclass of SlicedFile listed in formats/sliced.cpp; formats/cxdlp.cpp is
// the reference.
#pragma once

#include <QImage>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <array>
#include <functional>
#include <memory>
#include <stdexcept>
#include <vector>

namespace resindeck {

// Not a file of this format, damaged, or using a feature ResinDeck cannot read (encryption).
class FormatError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// The layers downsampled into a boolean grid cropped to the model, for the 3D view.
struct Voxels {
    std::array<int, 3> shape{};        // z, y, x
    std::vector<std::uint8_t> cells;   // shape[0] * shape[1] * shape[2], 1 = solid, z-major
    std::array<double, 3> voxel{};     // size of one cell in mm: x, y, z
    std::array<double, 3> origin{};    // position of cell (0, 0, 0) on the plate in mm: x, y, z
    int layersPerVoxel = 1;
    std::array<double, 3> bed{};       // plate size in mm: x, y, z

    bool empty() const { return cells.empty(); }
    bool at(int z, int y, int x) const {
        return cells[(static_cast<std::size_t>(z) * static_cast<std::size_t>(shape[1]) + static_cast<std::size_t>(y)) *
                         static_cast<std::size_t>(shape[2]) +
                     static_cast<std::size_t>(x)] != 0;
    }
};

// Implementations must allow several threads to read layers at once.
class SlicedFile {
public:
    using Progress = std::function<void(double fraction)>;

    virtual ~SlicedFile() = default;

    // Header values shown in the interface (the same keys as the Python version).
    virtual QJsonObject info() const = 0;
    virtual int width() const = 0;     // layer resolution in pixels
    virtual int height() const = 0;
    virtual std::array<double, 3> bed() const = 0;  // plate size in mm: x, y, z
    virtual double layerHeight() const = 0;         // mm
    virtual int layerCount() const = 0;
    virtual double layerZ(int index) const = 0;     // mm
    virtual int previewCount() const = 0;
    // A thumbnail; a negative index counts from the end (-1: the largest).
    virtual QImage preview(int index = -1) const = 0;
    // One layer as an 8-bit gray image: 0 = no light, 255 = full exposure.
    virtual QImage layer(int index) const = 0;

    // Size of one pixel on the plate in mm: x, y.
    std::array<double, 2> pixelMm() const;
    // Every `step` pixels in x and y, and as many layers as make the cells roughly cubic.
    Voxels voxels(int step = 6, const Progress &progress = {}) const;
};

// The 3D view's payload as Deck::voxels() builds it (uint32 meta length, JSON meta, packed bits),
// back into a grid. Empty for an empty or malformed payload.
Voxels decodeVoxels(const QByteArray &payload);

// The extensions with a reader, lower case: {".cxdlpv4", ".cxdlp"}.
QStringList slicedExtensions();
bool isSlicedFile(const QString &path);
// Throws FormatError if the extension has no reader or the file cannot be read.
std::unique_ptr<SlicedFile> loadSliced(const QString &path);

}  // namespace resindeck
