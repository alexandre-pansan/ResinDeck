// The 3D view of a sliced file: its voxel grid drawn by the GPU through Qt RHI (Vulkan, Direct3D,
// Metal or OpenGL, whichever the system has). Drag to orbit, right-drag to pan, wheel to zoom,
// double-click to reset the view.
#pragma once

#include "formats/sliced.h"

#include <QPointF>
#include <QRhiWidget>
#include <QVector3D>

#include <array>
#include <memory>
#include <vector>

class QRhiBuffer;
class QRhiGraphicsPipeline;
class QRhiShaderResourceBindings;

namespace resindeck::ui {

// Triangles for the outside faces of the solid cells (a face between two solid cells is hidden).
struct Mesh {
    std::vector<float> vertices;  // x, y, z, nx, ny, nz per vertex, in mm on the plate
    std::vector<quint32> indices;
    std::array<double, 3> bed{};
    double top = 0;  // highest point of the model, mm
    bool empty() const { return indices.empty(); }
};
Mesh buildMesh(const Voxels &voxels);

class Viewer3D : public QRhiWidget {
    Q_OBJECT
public:
    explicit Viewer3D(QWidget *parent = nullptr);
    ~Viewer3D() override;

    void setMesh(Mesh mesh);
    void clear();
    // Cuts the model above this height (mm); a negative value shows all of it.
    void setClipZ(double z);
    void resetView();

protected:
    void initialize(QRhiCommandBuffer *cb) override;
    void render(QRhiCommandBuffer *cb) override;
    void releaseResources() override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseDoubleClickEvent(QMouseEvent *e) override;
    void wheelEvent(QWheelEvent *e) override;

private:
    void buildLines();
    QMatrix4x4 viewProjection() const;

    Mesh mesh_;
    std::vector<float> lines_;  // the plate: grid and outline, same layout with a zero normal
    bool dirty_ = true;
    double clipZ_ = -1;

    float yaw_ = -35, pitch_ = 32, distance_ = 300;
    QVector3D target_;
    QPointF lastMouse_;

    QRhi *rhi_ = nullptr;
    std::unique_ptr<QRhiBuffer> vertices_, indices_, lineBuffer_, uniforms_;
    std::unique_ptr<QRhiShaderResourceBindings> bindings_;
    std::unique_ptr<QRhiGraphicsPipeline> meshPipeline_, linePipeline_;
    quint32 indexCount_ = 0, lineCount_ = 0;
};

}  // namespace resindeck::ui
