// Triangle meshes of a sliced file, for the 3D view.
//
// Three kinds:
// - voxelMesh(): the outside faces of the coarse voxel grid. Instant, blocky.
// - printedMesh(): the model as printed: one block per pixel and layer, exactly, drawn by its outside
//   faces (neighbouring faces merged). Exact, but too large for big organic models.
// - surfaceMesh(): a smooth surface through the layer images themselves (naive surface nets). The
//   layers' anti-aliasing places the edge between pixels, so the mesh follows the printed shape
//   within a fraction of a pixel. It reads the layers two at a time, so memory stays small.
#pragma once

#include "formats/sliced.h"

#include <array>
#include <atomic>
#include <functional>
#include <limits>
#include <vector>

namespace resindeck {

struct Mesh {
    std::vector<float> vertices;  // x, y, z, nx, ny, nz per vertex, in mm on the plate (y towards the back)
    std::vector<quint32> indices;
    std::array<double, 3> bed{};
    double top = 0;    // highest point of the model, mm
    int step = 0;      // surfaceMesh: pixels per sample in x and y
    int layerStep = 0; // surfaceMesh: layers per sample in z
    qint64 samples = 0; // surfaceMesh: samples visited
    bool tooLarge = false;  // printedMesh: over SurfaceOptions::maxTriangles, so empty
    bool empty() const { return indices.empty(); }
    std::size_t vertexCount() const { return vertices.size() / 6; }
};

// Over maxTriangles, the grid is made coarser (2x2x2 cells into one) until it fits.
Mesh voxelMesh(const Voxels &voxels, std::size_t maxTriangles = std::numeric_limits<std::size_t>::max());
// The grid with factor x factor x factor cells merged into one, solid if any of them is.
Voxels coarser(const Voxels &voxels, int factor);

struct SurfaceOptions {
    // The part of the file that has the model, in pixels and layers (from the voxel grid, see
    // modelBounds()); the whole file when empty.
    std::array<int, 4> pixels{};  // x0, y0, x1, y1 (exclusive)
    std::array<int, 2> layers{};  // first, last (exclusive)
    // Upper limit of samples to visit (x * y * z): sets how many pixels and layers make one sample.
    qint64 sampleBudget = 120'000'000;
    // Upper limit of triangles (GPU memory: about 24 bytes each). A larger surface is extracted again
    // on a coarser grid.
    std::size_t maxTriangles = 8'000'000;
    std::function<void(double fraction)> progress;
    const std::atomic<bool> *cancel = nullptr;  // stops early, returning an empty mesh
};

// The model's bounding box from the voxel grid, as SurfaceOptions pixels and layers, with a margin.
void modelBounds(const SlicedFile &file, const Voxels &voxels, SurfaceOptions &options);

Mesh surfaceMesh(const SlicedFile &file, const SurfaceOptions &options = {});

// The model as printed (pixels above half gray are solid). Uses pixels, layers, maxTriangles, progress
// and cancel from the options; over maxTriangles it stops and returns an empty mesh with tooLarge set.
// It stops early too, once a tenth of the layers shows the whole would be well over.
Mesh printedMesh(const SlicedFile &file, const SurfaceOptions &options = {});

}  // namespace resindeck
