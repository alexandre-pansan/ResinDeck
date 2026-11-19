// The 3D view of a sliced file: its mesh (see core/mesh) drawn by the GPU through Qt RHI (Vulkan, Direct3D,
// Metal or OpenGL, whichever the system has). Drag to orbit, right-drag to pan, wheel to zoom,
// double-click to reset the view.
#pragma once

#include "mesh/mesh.h"

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

class Viewer3D : public QRhiWidget {
    Q_OBJECT
public:
    explicit Viewer3D(QWidget *parent = nullptr);
    ~Viewer3D() override;

    // keepView: the same file in more detail, so leave the camera where the user put it
    void setMesh(Mesh mesh, bool keepView = false);
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
    QVector3D eye() const;
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
