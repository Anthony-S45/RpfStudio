#pragma once
#include "Drawable.h"
#include "Viewers.h"

#include <QMatrix4x4>
#include <QOpenGLBuffer>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLTexture>
#include <QOpenGLVertexArrayObject>
#include <QOpenGLWidget>
#include <memory>

class QComboBox;
class QCheckBox;
class QLabel;

class GLModelView : public QOpenGLWidget, protected QOpenGLFunctions {
    Q_OBJECT
public:
    explicit GLModelView(QWidget* parent = nullptr);
    ~GLModelView() override;
    void setModel(const rs::DrawableModel3D* m, const rs::TextureDictionary* extraTextures);
    void setWireframe(bool w) { m_wire = w; update(); }
    void setTextured(bool t) { m_textured = t; update(); }
    void resetCamera();
protected:
    void initializeGL() override;
    void paintGL() override;
    void resizeGL(int w, int h) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;
private:
    struct GpuMesh {
        std::unique_ptr<QOpenGLVertexArrayObject> vao;
        QOpenGLBuffer vbo{QOpenGLBuffer::VertexBuffer}, ibo{QOpenGLBuffer::IndexBuffer}, lbo{QOpenGLBuffer::IndexBuffer};
        int indexCount = 0, lineCount = 0;
        QOpenGLTexture* tex = nullptr;
    };
    void upload();
    void releaseGpu();
    const rs::DrawableModel3D* m_model = nullptr;
    const rs::TextureDictionary* m_extra = nullptr;
    bool m_dirty = false, m_wire = false, m_textured = true, m_glReady = false;
    std::vector<std::unique_ptr<GpuMesh>> m_meshes;
    std::vector<std::unique_ptr<QOpenGLTexture>> m_textures;
    std::unique_ptr<QOpenGLShaderProgram> m_prog;
    QVector3D m_center;
    float m_radius = 1, m_yaw = 45, m_pitch = 20, m_dist = 3;
    QVector3D m_pan;
    QPoint m_last;
};

class ModelViewer : public Viewer {
    Q_OBJECT
public:
    explicit ModelViewer(QWidget* parent = nullptr);
    bool load(const QByteArray& data, const QString& ext, QString* err);
    void addActions(QToolBar* tb) override;
private:
    void select(int i);
    void exportObj();
    void showTextures();
    std::vector<rs::DrawableModel3D> m_models;
    GLModelView* m_view;
    QComboBox* m_pick;
    QLabel* m_info;
    int m_current = 0;
};
