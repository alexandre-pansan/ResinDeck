#include "viewer3d.h"

#include <QFile>
#include <QMouseEvent>
#include <QWheelEvent>
#include <rhi/qrhi.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace resindeck::ui {

namespace {

QShader loadShader(const QString &name) {
    QFile f(":/shaders/" + name + ".qsb");
    return f.open(QIODevice::ReadOnly) ? QShader::fromSerialized(f.readAll()) : QShader();
}

constexpr quint32 kUniformSize = 144;  // mat4 + 4 vec4 + float, std140

}  // namespace

Viewer3D::Viewer3D(QWidget *parent) : QRhiWidget(parent) {
    setMinimumSize(200, 200);
    setSampleCount(4);  // smooth edges
    setFocusPolicy(Qt::StrongFocus);
}

Viewer3D::~Viewer3D() = default;

void Viewer3D::setMesh(Mesh mesh, bool keepView) {
    mesh_ = std::move(mesh);
    dirty_ = true;
    if (keepView) {
        update();
        return;
    }
    clipZ_ = -1;
    resetView();
}

void Viewer3D::clear() { setMesh({}); }

void Viewer3D::setClipZ(double z) {
    clipZ_ = z;
    update();
}

void Viewer3D::resetView() {
    const auto b = mesh_.bed[0] > 0 ? mesh_.bed : std::array<double, 3>{130, 80, 160};
    // frame the model when there is one, the plate otherwise
    QVector3D lo{0, 0, 0}, hi{float(b[0]), float(b[1]), float(std::max(mesh_.top, 10.0))};
    if (!mesh_.empty()) {
        lo = QVector3D(1e9f, 1e9f, 1e9f);
        hi = -lo;
        for (std::size_t i = 0; i < mesh_.vertices.size(); i += 6) {
            const QVector3D p(mesh_.vertices[i], mesh_.vertices[i + 1], mesh_.vertices[i + 2]);
            lo = QVector3D(std::min(lo.x(), p.x()), std::min(lo.y(), p.y()), std::min(lo.z(), p.z()));
            hi = QVector3D(std::max(hi.x(), p.x()), std::max(hi.y(), p.y()), std::max(hi.z(), p.z()));
        }
    }
    target_ = (lo + hi) / 2;
    const float radius = std::max((hi - lo).length() / 2, 5.0f);
    distance_ = radius / std::sin(qDegreesToRadians(20.0f)) * 1.05f;  // the 40° field of view fits it
    yaw_ = -35;
    pitch_ = 32;
    update();
}

void Viewer3D::buildLines() {
    lines_.clear();
    const auto b = mesh_.bed[0] > 0 ? mesh_.bed : std::array<double, 3>{130, 80, 160};
    const auto bx = float(b[0]), by = float(b[1]), bz = float(b[2]);
    auto line = [&](float x1, float y1, float z1, float x2, float y2, float z2) {
        lines_.insert(lines_.end(), {x1, y1, z1, 0, 0, 0, x2, y2, z2, 0, 0, 0});
    };
    for (float x = 0; x <= bx + 0.01f; x += 10) line(x, 0, 0, x, by, 0);
    for (float y = 0; y <= by + 0.01f; y += 10) line(0, y, 0, bx, y, 0);
    line(bx, 0, 0, bx, by, 0);
    line(0, by, 0, bx, by, 0);
    for (auto [x, y] : {std::pair{0.f, 0.f}, {bx, 0.f}, {bx, by}, {0.f, by}}) line(x, y, 0, x, y, bz);
    line(0, 0, bz, bx, 0, bz);
    line(bx, 0, bz, bx, by, bz);
    line(bx, by, bz, 0, by, bz);
    line(0, by, bz, 0, 0, bz);
}

QVector3D Viewer3D::eye() const {
    const float yaw = qDegreesToRadians(yaw_), pitch = qDegreesToRadians(pitch_);
    return target_ + distance_ * QVector3D(std::cos(pitch) * std::sin(yaw), -std::cos(pitch) * std::cos(yaw), std::sin(pitch));
}

QMatrix4x4 Viewer3D::viewProjection() const {
    const QVector3D eye = this->eye();
    QMatrix4x4 m = rhi_ ? rhi_->clipSpaceCorrMatrix() : QMatrix4x4();
    const QSize size = renderTarget() ? renderTarget()->pixelSize() : this->size();
    m.perspective(40.0f, float(size.width()) / float(std::max(size.height(), 1)), distance_ * 0.01f, distance_ * 10.0f);
    QMatrix4x4 view;
    view.lookAt(eye, target_, QVector3D(0, 0, 1));
    return m * view;
}

void Viewer3D::initialize(QRhiCommandBuffer *) {
    if (rhi_ != rhi()) {
        rhi_ = rhi();
        meshPipeline_.reset();
        linePipeline_.reset();
        bindings_.reset();
        uniforms_.reset();
        dirty_ = true;
    }
    if (!uniforms_) {
        uniforms_.reset(rhi_->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, kUniformSize));
        uniforms_->create();
        bindings_.reset(rhi_->newShaderResourceBindings());
        bindings_->setBindings({QRhiShaderResourceBinding::uniformBuffer(
            0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage, uniforms_.get())});
        bindings_->create();
    }
    if (!meshPipeline_) {
        QRhiVertexInputLayout layout;
        layout.setBindings({{6 * sizeof(float)}});
        layout.setAttributes({{0, 0, QRhiVertexInputAttribute::Float3, 0}, {0, 1, QRhiVertexInputAttribute::Float3, 3 * sizeof(float)}});
        auto make = [&](QRhiGraphicsPipeline::Topology topology) {
            std::unique_ptr<QRhiGraphicsPipeline> p(rhi_->newGraphicsPipeline());
            p->setTopology(topology);
            p->setDepthTest(true);
            p->setDepthWrite(true);
            p->setSampleCount(renderTarget()->sampleCount());
            p->setShaderStages({{QRhiShaderStage::Vertex, loadShader("voxel.vert")}, {QRhiShaderStage::Fragment, loadShader("voxel.frag")}});
            p->setVertexInputLayout(layout);
            p->setShaderResourceBindings(bindings_.get());
            p->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());
            p->create();
            return p;
        };
        meshPipeline_ = make(QRhiGraphicsPipeline::Triangles);
        linePipeline_ = make(QRhiGraphicsPipeline::Lines);
    }
}

void Viewer3D::render(QRhiCommandBuffer *cb) {
    QRhiResourceUpdateBatch *u = rhi_->nextResourceUpdateBatch();
    if (dirty_) {
        dirty_ = false;
        buildLines();
        vertices_.reset();
        indices_.reset();
        indexCount_ = static_cast<quint32>(mesh_.indices.size());
        if (indexCount_) {
            const auto vbytes = static_cast<quint32>(mesh_.vertices.size() * sizeof(float));
            const auto ibytes = static_cast<quint32>(mesh_.indices.size() * sizeof(quint32));
            vertices_.reset(rhi_->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, vbytes));
            indices_.reset(rhi_->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::IndexBuffer, ibytes));
            vertices_->create();
            indices_->create();
            u->uploadStaticBuffer(vertices_.get(), mesh_.vertices.data());
            u->uploadStaticBuffer(indices_.get(), mesh_.indices.data());
        }
        lineCount_ = static_cast<quint32>(lines_.size() / 6);
        lineBuffer_.reset(rhi_->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, static_cast<quint32>(lines_.size() * sizeof(float))));
        lineBuffer_->create();
        u->uploadStaticBuffer(lineBuffer_.get(), lines_.data());
    }

    float frame[kUniformSize / sizeof(float)] = {};
    const QMatrix4x4 mvp = viewProjection();
    std::memcpy(frame, mvp.constData(), 16 * sizeof(float));
    const float model[4] = {0.24f, 0.55f, 0.95f, 1.0f};  // the web viewer's blue
    const float line[4] = {0.22f, 0.25f, 0.30f, 1.0f};
    const QVector3D light = QVector3D(0.4f, -0.6f, 0.7f).normalized();
    std::memcpy(frame + 16, model, sizeof model);
    std::memcpy(frame + 20, line, sizeof line);
    frame[24] = light.x(), frame[25] = light.y(), frame[26] = light.z();
    const QVector3D eye = this->eye();
    frame[28] = eye.x(), frame[29] = eye.y(), frame[30] = eye.z();
    frame[32] = clipZ_ < 0 ? 1e9f : static_cast<float>(clipZ_);
    u->updateDynamicBuffer(uniforms_.get(), 0, kUniformSize, frame);

    const QSize size = renderTarget()->pixelSize();
    cb->beginPass(renderTarget(), QColor(18, 20, 24), {1.0f, 0}, u);
    const QRhiViewport viewport(0, 0, float(size.width()), float(size.height()));
    if (lineCount_) {
        cb->setGraphicsPipeline(linePipeline_.get());
        cb->setViewport(viewport);
        cb->setShaderResources();
        const QRhiCommandBuffer::VertexInput in(lineBuffer_.get(), 0);
        cb->setVertexInput(0, 1, &in);
        cb->draw(lineCount_);
    }
    if (indexCount_) {
        cb->setGraphicsPipeline(meshPipeline_.get());
        cb->setViewport(viewport);
        cb->setShaderResources();
        const QRhiCommandBuffer::VertexInput in(vertices_.get(), 0);
        cb->setVertexInput(0, 1, &in, indices_.get(), 0, QRhiCommandBuffer::IndexUInt32);
        cb->drawIndexed(indexCount_);
    }
    cb->endPass();
}

void Viewer3D::releaseResources() {
    meshPipeline_.reset();
    linePipeline_.reset();
    bindings_.reset();
    uniforms_.reset();
    vertices_.reset();
    indices_.reset();
    lineBuffer_.reset();
    rhi_ = nullptr;
    dirty_ = true;
}

void Viewer3D::mousePressEvent(QMouseEvent *e) { lastMouse_ = e->position(); }

void Viewer3D::mouseMoveEvent(QMouseEvent *e) {
    const QPointF d = e->position() - lastMouse_;
    lastMouse_ = e->position();
    if (e->buttons() & Qt::LeftButton) {
        yaw_ -= float(d.x()) * 0.4f;
        pitch_ = std::clamp(pitch_ + float(d.y()) * 0.4f, -5.0f, 89.0f);
    } else if (e->buttons() & (Qt::RightButton | Qt::MiddleButton)) {
        const float scale = distance_ / float(std::max(height(), 1)) * 0.8f;
        const float yaw = qDegreesToRadians(yaw_);
        const QVector3D right(std::cos(yaw), std::sin(yaw), 0);
        target_ += -right * float(d.x()) * scale + QVector3D(0, 0, float(d.y()) * scale);
    }
    update();
}

void Viewer3D::mouseDoubleClickEvent(QMouseEvent *) { resetView(); }

void Viewer3D::wheelEvent(QWheelEvent *e) {
    distance_ = std::clamp(distance_ * std::pow(0.999f, float(e->angleDelta().y())), 10.0f, 2000.0f);
    update();
}

}  // namespace resindeck::ui
