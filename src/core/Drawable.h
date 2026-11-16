// Geometry extraction from drawables (.ydr), drawable dictionaries (.ydd)
// and fragments (.yft) for preview and OBJ export.
#pragma once
#include "Ytd.h"
#include <QString>
#include <vector>

namespace rs {

struct Mesh {
    std::vector<float> positions;   // xyz
    std::vector<float> normals;     // xyz (may be empty)
    std::vector<float> uvs;         // uv  (may be empty)
    std::vector<uint32_t> indices;  // triangle list
    int shaderIndex = -1;
    QString diffuseTexture;         // texture name referenced by the shader (if any)
};

struct DrawableModel3D {
    QString name;
    std::vector<Mesh> meshes;
    float bbMin[3] = {0, 0, 0}, bbMax[3] = {0, 0, 0};
    TextureDictionary embeddedTextures;
    bool hasEmbeddedTextures = false;
    QString lodUsed;
};

// ext: "ydr", "ydd" or "yft". Returns one model per drawable.
bool loadDrawables(const QByteArray& rsc7, const QString& ext, std::vector<DrawableModel3D>& out, QString* err);

// Wavefront OBJ (+ optional MTL file name reference).
QByteArray exportObj(const DrawableModel3D& m, const QString& mtlName = QString());
QByteArray exportMtl(const DrawableModel3D& m);

} // namespace rs
