#include "ModelViewer.h"
#include "TextureViewer.h"

#include <QStyle>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QMouseEvent>
#include <QToolBar>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <cmath>
#include <map>

using namespace rs;

static const char* kVert = R"(#version 330 core
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aNrm;
layout(location=2) in vec2 aUv;
uniform mat4 uMvp;
out vec3 vNrm;
out vec2 vUv;
void main() { vNrm = aNrm; vUv = aUv; gl_Position = uMvp * vec4(aPos, 1.0); }
)";

static const char* kFrag = R"(#version 330 core
in vec3 vNrm;
in vec2 vUv;
uniform sampler2D uTex;
uniform int uUseTex;
uniform vec3 uColor;
uniform vec3 uLight;
uniform int uFlat;
out vec4 frag;
void main() {
  if (uFlat == 1) { frag = vec4(uColor, 1.0); return; }
  vec3 n = normalize(vNrm);
  float d = abs(dot(n, normalize(uLight)));
  vec4 base = uUseTex == 1 ? texture(uTex, vUv) : vec4(uColor, 1.0);
  if (uUseTex == 1 && base.a < 0.3) discard;
  frag = vec4(base.rgb * (0.35 + 0.65 * d), 1.0);
}
)";

GLModelView::GLModelView(QWidget* parent) : QOpenGLWidget(parent)
{
    QSurfaceFormat f;
    f.setVersion(3, 3);
    f.setProfile(QSurfaceFormat::CoreProfile);
    f.setDepthBufferSize(24);
    f.setSamples(4);
    setFormat(f);
    setMinimumSize(320, 240);
}

GLModelView::~GLModelView()
{
    makeCurrent();
    releaseGpu();
    m_prog.reset();
    doneCurrent();
}

void GLModelView::releaseGpu()
{
    for (auto& m : m_meshes) {
        m->vbo.destroy();
        m->ibo.destroy();
        m->lbo.destroy();
        if (m->vao) m->vao->destroy();
    }
    m_meshes.clear();
    m_textures.clear();
}

void GLModelView::setModel(const DrawableModel3D* m, const TextureDictionary* extra)
{
    m_model = m;
    m_extra = extra;
    m_dirty = true;
    resetCamera();
    update();
}

void GLModelView::resetCamera()
{
    if (!m_model) return;
    QVector3D mn(1e30f, 1e30f, 1e30f), mx(-1e30f, -1e30f, -1e30f);
    for (auto& mesh : m_model->meshes)
        for (size_t i = 0; i + 2 < mesh.positions.size(); i += 3) {
            QVector3D p(mesh.positions[i], mesh.positions[i + 1], mesh.positions[i + 2]);
            for (int k = 0; k < 3; ++k) { mn[k] = std::min(mn[k], p[k]); mx[k] = std::max(mx[k], p[k]); }
        }
    if (mn.x() > mx.x()) { mn = QVector3D(-1, -1, -1); mx = QVector3D(1, 1, 1); }
    m_center = (mn + mx) * 0.5f;
    m_radius = std::max(0.01f, (mx - mn).length() * 0.5f);
    m_dist = m_radius * 2.6f;
    m_yaw = 45;
    m_pitch = 20;
    m_pan = QVector3D();
    update();
}

void GLModelView::initializeGL()
{
    initializeOpenGLFunctions();
    m_prog = std::make_unique<QOpenGLShaderProgram>();
    bool ok = m_prog->addShaderFromSourceCode(QOpenGLShader::Vertex, kVert) &&
              m_prog->addShaderFromSourceCode(QOpenGLShader::Fragment, kFrag) && m_prog->link();
    m_glReady = ok;
    if (!ok) qWarning("Model viewer shader error: %s", qPrintable(m_prog->log()));
    m_dirty = true;
}

void GLModelView::upload()
{
    releaseGpu();
    m_dirty = false;
    if (!m_model) return;
    std::map<QString, QOpenGLTexture*> texByName;
    auto addDict = [&](const TextureDictionary* d) {
        if (!d) return;
        for (auto& t : d->textures) {
            QString key = t.name.toLower();
            if (texByName.count(key)) continue;
            QImage img = decodeTexture(t.image);
            if (img.isNull()) continue;
            auto tex = std::make_unique<QOpenGLTexture>(img.mirrored(false, false));
            tex->setMinificationFilter(QOpenGLTexture::LinearMipMapLinear);
            tex->setMagnificationFilter(QOpenGLTexture::Linear);
            tex->setWrapMode(QOpenGLTexture::Repeat);
            texByName[key] = tex.get();
            m_textures.push_back(std::move(tex));
        }
    };
    if (m_model->hasEmbeddedTextures) addDict(&m_model->embeddedTextures);
    addDict(m_extra);

    for (const Mesh& mesh : m_model->meshes) {
        size_t vc = mesh.positions.size() / 3;
        std::vector<float> normals = mesh.normals;
        if (normals.size() != vc * 3) {  // compute smooth normals
            normals.assign(vc * 3, 0.f);
            for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
                uint32_t a = mesh.indices[i], b = mesh.indices[i + 1], c = mesh.indices[i + 2];
                QVector3D pa(mesh.positions[a*3], mesh.positions[a*3+1], mesh.positions[a*3+2]);
                QVector3D pb(mesh.positions[b*3], mesh.positions[b*3+1], mesh.positions[b*3+2]);
                QVector3D pc(mesh.positions[c*3], mesh.positions[c*3+1], mesh.positions[c*3+2]);
                QVector3D n = QVector3D::crossProduct(pb - pa, pc - pa);
                for (uint32_t v : {a, b, c}) for (int k = 0; k < 3; ++k) normals[v * 3 + k] += n[k];
            }
        }
        std::vector<float> inter(vc * 8);
        for (size_t i = 0; i < vc; ++i) {
            for (int k = 0; k < 3; ++k) { inter[i*8+k] = mesh.positions[i*3+k]; inter[i*8+3+k] = normals[i*3+k]; }
            inter[i*8+6] = mesh.uvs.size() == vc * 2 ? mesh.uvs[i*2] : 0.f;
            inter[i*8+7] = mesh.uvs.size() == vc * 2 ? mesh.uvs[i*2+1] : 0.f;
        }
        std::vector<uint32_t> lines;
        lines.reserve(mesh.indices.size() * 2);
        for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3)
            for (int k = 0; k < 3; ++k) { lines.push_back(mesh.indices[i + k]); lines.push_back(mesh.indices[i + (k + 1) % 3]); }

        auto g = std::make_unique<GpuMesh>();
        g->vao = std::make_unique<QOpenGLVertexArrayObject>();
        g->vao->create();
        g->vao->bind();
        g->vbo.create();
        g->vbo.bind();
        g->vbo.allocate(inter.data(), int(inter.size() * sizeof(float)));
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 32, nullptr);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 32, reinterpret_cast<void*>(12));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 32, reinterpret_cast<void*>(24));
        g->ibo.create();
        g->ibo.bind();
        g->ibo.allocate(mesh.indices.data(), int(mesh.indices.size() * 4));
        g->indexCount = int(mesh.indices.size());
        g->vao->release();
        g->lbo.create();
        g->lbo.bind();
        g->lbo.allocate(lines.data(), int(lines.size() * 4));
        g->lineCount = int(lines.size());
        g->lbo.release();
        auto it = texByName.find(mesh.diffuseTexture.toLower());
        g->tex = it == texByName.end() ? nullptr : it->second;
        m_meshes.push_back(std::move(g));
    }
}

void GLModelView::resizeGL(int, int) {}

void GLModelView::paintGL()
{
    glClearColor(0.18f, 0.19f, 0.22f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    if (!m_glReady) return;
    if (m_dirty) upload();
    if (m_meshes.empty()) return;
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    QMatrix4x4 proj, view;
    proj.perspective(45.f, float(width()) / std::max(1, height()), m_radius * 0.01f, m_radius * 100.f);
    float yaw = qDegreesToRadians(m_yaw), pitch = qDegreesToRadians(m_pitch);
    QVector3D target = m_center + m_pan;
    QVector3D eye = target + m_dist * QVector3D(std::cos(pitch) * std::cos(yaw), std::cos(pitch) * std::sin(yaw), std::sin(pitch));
    view.lookAt(eye, target, QVector3D(0, 0, 1));  // GTA is Z-up
    m_prog->bind();
    m_prog->setUniformValue("uMvp", proj * view);
    m_prog->setUniformValue("uLight", (eye - target).normalized() + QVector3D(0, 0, 0.5f));
    m_prog->setUniformValue("uTex", 0);
    const QVector3D palette[] = {{0.75f, 0.75f, 0.78f}, {0.7f, 0.78f, 0.9f}, {0.9f, 0.8f, 0.65f}, {0.7f, 0.88f, 0.72f}};
    int i = 0;
    for (auto& g : m_meshes) {
        bool tex = m_textured && g->tex;
        m_prog->setUniformValue("uUseTex", tex ? 1 : 0);
        m_prog->setUniformValue("uFlat", 0);
        m_prog->setUniformValue("uColor", palette[i++ % 4]);
        if (tex) { glActiveTexture(GL_TEXTURE0); g->tex->bind(); }
        g->vao->bind();
        if (!m_wire) glDrawElements(GL_TRIANGLES, g->indexCount, GL_UNSIGNED_INT, nullptr);
        if (m_wire) {
            m_prog->setUniformValue("uFlat", 1);
            m_prog->setUniformValue("uColor", QVector3D(0.9f, 0.9f, 0.9f));
            g->lbo.bind();
            glDrawElements(GL_LINES, g->lineCount, GL_UNSIGNED_INT, nullptr);
            g->ibo.bind();
        }
        g->vao->release();
        if (tex) g->tex->release();
    }
    m_prog->release();
}

void GLModelView::mousePressEvent(QMouseEvent* e) { m_last = e->pos(); }

void GLModelView::mouseMoveEvent(QMouseEvent* e)
{
    QPoint d = e->pos() - m_last;
    m_last = e->pos();
    if (e->buttons() & Qt::LeftButton) {
        m_yaw -= d.x() * 0.5f;
        m_pitch = std::clamp(m_pitch + d.y() * 0.5f, -89.f, 89.f);
    } else if (e->buttons() & (Qt::RightButton | Qt::MiddleButton)) {
        float yaw = qDegreesToRadians(m_yaw);
        QVector3D right(-std::sin(yaw), std::cos(yaw), 0);
        float s = m_dist * 0.002f;
        m_pan += -right * d.x() * s + QVector3D(0, 0, 1) * d.y() * s;
    }
    update();
}

void GLModelView::wheelEvent(QWheelEvent* e)
{
    m_dist *= std::pow(0.9f, e->angleDelta().y() / 120.f);
    m_dist = std::clamp(m_dist, m_radius * 0.05f, m_radius * 50.f);
    update();
}

// ------------------------------------------------------------------ ModelViewer

ModelViewer::ModelViewer(QWidget* parent) : Viewer(parent)
{
    auto* l = new QVBoxLayout(this);
    l->setContentsMargins(0, 0, 0, 0);
    auto* bar = new QHBoxLayout;
    bar->setContentsMargins(4, 4, 4, 0);
    m_pick = new QComboBox;
    m_pick->setMinimumWidth(200);
    auto* wire = new QCheckBox("Wireframe");
    auto* tex = new QCheckBox("Textures");
    tex->setChecked(true);
    m_info = new QLabel;
    bar->addWidget(new QLabel("Drawable:"));
    bar->addWidget(m_pick);
    bar->addWidget(wire);
    bar->addWidget(tex);
    bar->addStretch();
    bar->addWidget(m_info);
    l->addLayout(bar);
    m_view = new GLModelView(this);
    l->addWidget(m_view, 1);
    auto* hint = new QLabel("  Left-drag: orbit   Right/middle-drag: pan   Wheel: zoom");
    hint->setEnabled(false);
    l->addWidget(hint);
    connect(wire, &QCheckBox::toggled, m_view, &GLModelView::setWireframe);
    connect(tex, &QCheckBox::toggled, m_view, &GLModelView::setTextured);
    connect(m_pick, qOverload<int>(&QComboBox::currentIndexChanged), this, &ModelViewer::select);
}

bool ModelViewer::load(const QByteArray& data, const QString& ext, QString* err)
{
    if (!loadDrawables(data, ext, m_models, err)) return false;
    m_pick->blockSignals(true);
    for (size_t i = 0; i < m_models.size(); ++i)
        m_pick->addItem(m_models[i].name.isEmpty() ? QStringLiteral("Drawable %1").arg(i) : m_models[i].name);
    m_pick->blockSignals(false);
    m_pick->setEnabled(m_models.size() > 1);
    select(0);
    return true;
}

void ModelViewer::select(int i)
{
    if (i < 0 || i >= int(m_models.size())) return;
    m_current = i;
    const auto& m = m_models[size_t(i)];
    size_t tris = 0, verts = 0;
    for (auto& mesh : m.meshes) { tris += mesh.indices.size() / 3; verts += mesh.positions.size() / 3; }
    m_info->setText(QStringLiteral("LOD %1 · %2 meshes · %3 vertices · %4 triangles%5")
                        .arg(m.lodUsed).arg(m.meshes.size()).arg(verts).arg(tris)
                        .arg(m.hasEmbeddedTextures ? QStringLiteral(" · %1 embedded textures").arg(m.embeddedTextures.textures.size()) : QString()));
    m_view->setModel(&m, nullptr);
}

void ModelViewer::addActions(QToolBar* tb)
{
    tb->addAction("Export OBJ...", this, &ModelViewer::exportObj);
    tb->addAction("Embedded textures...", this, &ModelViewer::showTextures);
    tb->addAction("Reset view", m_view, &GLModelView::resetCamera);
}

void ModelViewer::exportObj()
{
    const auto& m = m_models[size_t(m_current)];
    QString name = m.name.isEmpty() ? "model" : m.name;
    QString p = QFileDialog::getSaveFileName(this, "Export OBJ", name + ".obj", "Wavefront OBJ (*.obj)");
    if (p.isEmpty()) return;
    QString mtl = QFileInfo(p).completeBaseName() + ".mtl";
    QFile f(p);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) { QMessageBox::warning(this, "Export", "Cannot write " + p); return; }
    f.write(rs::exportObj(m, mtl));
    QFile mf(QFileInfo(p).absolutePath() + "/" + mtl);
    if (mf.open(QIODevice::WriteOnly | QIODevice::Truncate)) mf.write(rs::exportMtl(m));
    int texCount = 0;
    if (m.hasEmbeddedTextures)
        for (auto& t : m.embeddedTextures.textures)
            if (decodeTexture(t.image).save(QFileInfo(p).absolutePath() + "/" + t.name + ".png")) ++texCount;
    QMessageBox::information(this, "Export", QStringLiteral("Exported %1 (+ %2 textures).").arg(p).arg(texCount));
}

void ModelViewer::showTextures()
{
    const auto& m = m_models[size_t(m_current)];
    if (!m.hasEmbeddedTextures || m.embeddedTextures.textures.empty()) {
        QMessageBox::information(this, "Textures", "This model has no embedded textures (it uses a separate .ytd).");
        return;
    }
    auto* v = new TextureViewer;
    v->setDictionary(m.embeddedTextures, false);
    FileHandle h;
    h.name = (m.name.isEmpty() ? "model" : m.name) + " textures";
    h.displayPath = h.name;
    auto* w = new ViewerWindow(h, v, window());
    w->show();
}
