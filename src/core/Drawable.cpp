#include "Drawable.h"
#include "BinaryIO.h"
#include "Crypto.h"

#include <QSet>
#include <cmath>

namespace rs {

namespace {

float halfToFloat(uint16_t h)
{
    uint32_t s = (h >> 15) & 1, e = (h >> 10) & 0x1F, m = h & 0x3FF;
    float v;
    if (e == 0) v = std::ldexp(float(m), -24);
    else if (e == 31) v = m ? NAN : INFINITY;
    else v = std::ldexp(float(m | 0x400), int(e) - 25);
    return s ? -v : v;
}

int componentSize(int type)
{
    switch (type) {
    case 1: return 4;   // Half2
    case 2: return 4;   // Float
    case 3: return 8;   // Half4
    case 5: return 8;   // Float2
    case 6: return 12;  // Float3
    case 7: return 16;  // Float4
    case 8: return 4;   // UByte4
    case 9: return 4;   // Colour
    case 10: return 4;  // RGBA8SNorm
    default: return 0;
    }
}

// Reads up to 3 floats from a vertex component.
int readComponent(const char* p, int type, float* out)
{
    switch (type) {
    case 1: out[0] = halfToFloat(rd16(p)); out[1] = halfToFloat(rd16(p + 2)); return 2;
    case 2: out[0] = rdf(p); return 1;
    case 3: for (int i = 0; i < 3; ++i) out[i] = halfToFloat(rd16(p + 2 * i)); return 3;
    case 5: out[0] = rdf(p); out[1] = rdf(p + 4); return 2;
    case 6: case 7: for (int i = 0; i < 3; ++i) out[i] = rdf(p + 4 * i); return 3;
    case 8: case 9: for (int i = 0; i < 3; ++i) out[i] = uint8_t(p[i]) / 127.5f - 1.f; return 3;
    case 10: for (int i = 0; i < 3; ++i) out[i] = int8_t(p[i]) / 127.f; return 3;
    default: return 0;
    }
}

struct ShaderInfo { QString diffuse; };

const uint32_t* diffuseHashList(int& n)
{
    static const uint32_t h[] = {joaat("diffusesampler"), joaat("diffusesampler2"), joaat("texturesampler_layer0"),
                                 joaat("diffusesamplerpoint"), joaat("texturesampler"), joaat("diffusetex"),
                                 joaat("diffusetexture"), joaat("platebgsampler")};
    n = int(sizeof(h) / sizeof(h[0]));
    return h;
}

// Gen9 (Enhanced) shaders: textures are referenced through a param-info table.
std::vector<ShaderInfo> readShadersGen9(const ResourceData& r, uint64_t shaderGroup)
{
    std::vector<ShaderInfo> out;
    if (!r.valid(shaderGroup, 0x20)) return out;
    uint64_t shadersPtr = r.u64(shaderGroup + 0x10);
    uint16_t count = r.u16(shaderGroup + 0x18);
    if (count > 1024 || !r.valid(shadersPtr, qint64(count) * 8)) return out;
    int nd = 0;
    const uint32_t* dh = diffuseHashList(nd);
    for (int i = 0; i < count; ++i) {
        ShaderInfo info;
        uint64_t sp = r.u64(shadersPtr + uint64_t(i) * 8);
        if (r.valid(sp, 0x40)) {
            uint64_t texRefs = r.u64(sp + 0x10);
            uint64_t infos = r.u64(sp + 0x20);
            if (r.valid(infos, 8)) {
                uint8_t numTex = r.u8(infos + 1), numParams = r.u8(infos + 4);
                QString first;
                for (int k = 0; k < numParams && r.valid(infos + 8 + uint64_t(k) * 8, 8); ++k) {
                    uint32_t hash = r.u32(infos + 8 + uint64_t(k) * 8);
                    uint32_t data = r.u32(infos + 12 + uint64_t(k) * 8);
                    if ((data & 3) != 0) continue;  // not a texture
                    uint32_t ti = (data >> 2) & 0xFF;
                    if (ti >= numTex || !r.valid(texRefs + ti * 8, 8)) continue;
                    uint64_t tex = r.u64(texRefs + ti * 8);
                    if (!r.valid(tex, 0x30)) continue;
                    QString name = r.cstr(r.u64(tex + 0x28));
                    if (name.isEmpty()) continue;
                    if (first.isEmpty()) first = name;
                    for (int d = 0; d < nd; ++d)
                        if (hash == dh[d] && info.diffuse.isEmpty()) info.diffuse = name;
                }
                if (info.diffuse.isEmpty()) info.diffuse = first;
            }
        }
        out.push_back(info);
    }
    return out;
}

std::vector<ShaderInfo> readShaders(const ResourceData& r, uint64_t shaderGroup)
{
    std::vector<ShaderInfo> out;
    if (!r.valid(shaderGroup, 0x40)) return out;
    uint64_t shadersPtr = r.u64(shaderGroup + 0x10);
    uint16_t count = r.u16(shaderGroup + 0x18);
    if (count > 1024 || !r.valid(shadersPtr, qint64(count) * 8)) return out;
    int nd = 0;
    const uint32_t* dh = diffuseHashList(nd);
    for (int i = 0; i < count; ++i) {
        ShaderInfo info;
        uint64_t sp = r.u64(shadersPtr + uint64_t(i) * 8);
        if (r.valid(sp, 0x30)) {
            uint64_t params = r.u64(sp);
            uint8_t pcount = r.u8(sp + 0x10);
            if (r.valid(params, qint64(pcount) * 16)) {
                qint64 embedded = 0;
                for (int k = 0; k < pcount; ++k) {
                    uint8_t dt = r.u8(params + uint64_t(k) * 16);
                    if (dt) embedded += 16 * dt;
                }
                uint64_t hashes = params + uint64_t(pcount) * 16 + uint64_t(embedded);
                QString firstTex;
                for (int k = 0; k < pcount; ++k) {
                    uint64_t pp = params + uint64_t(k) * 16;
                    if (r.u8(pp) != 0) continue;  // not a texture
                    uint64_t tex = r.u64(pp + 8);
                    if (!r.valid(tex, 0x30)) continue;
                    QString name = r.cstr(r.u64(tex + 0x28));
                    if (name.isEmpty()) continue;
                    uint32_t h = r.u32(hashes + uint64_t(k) * 4);
                    if (firstTex.isEmpty()) firstTex = name;
                    for (int d = 0; d < nd; ++d)
                        if (h == dh[d] && info.diffuse.isEmpty()) info.diffuse = name;
                }
                if (info.diffuse.isEmpty()) info.diffuse = firstTex;
            }
        }
        out.push_back(info);
    }
    return out;
}

// Reads 3 floats from a gen9 vertex element; returns component count.
int readGen9Element(const char* p, uint8_t fmt, float* out)
{
    switch (fmt) {
    case 2: case 6: for (int i = 0; i < 3; ++i) out[i] = rdf(p + 4 * i); return 3;      // R32G32B32(A32)_FLOAT
    case 10: for (int i = 0; i < 3; ++i) out[i] = halfToFloat(rd16(p + 2 * i)); return 3; // R16G16B16A16_FLOAT
    case 16: out[0] = rdf(p); out[1] = rdf(p + 4); return 2;                                // R32G32 (float2)
    case 34: out[0] = halfToFloat(rd16(p)); out[1] = halfToFloat(rd16(p + 2)); return 2;    // R16G16_FLOAT
    case 24: {                                                                              // R10G10B10A2
        uint32_t v = rd32(p);
        for (int i = 0; i < 3; ++i) out[i] = float((v >> (10 * i)) & 0x3FF) / 511.5f - 1.f;
        return 3;
    }
    case 28: case 30: for (int i = 0; i < 3; ++i) out[i] = uint8_t(p[i]) / 127.5f - 1.f; return 3;
    default: return 0;
    }
}

bool readGeometryGen9(const ResourceData& r, uint64_t gp, Mesh& m)
{
    if (!r.valid(gp, 0x98)) return false;
    uint64_t vb = r.u64(gp + 0x18), ib = r.u64(gp + 0x38);
    if (!r.valid(vb, 0x40) || !r.valid(ib, 0x20)) return false;
    uint32_t vcount = r.u32(vb + 0x08);
    uint16_t stride = r.u16(vb + 0x0C);
    uint64_t vdata = r.u64(vb + 0x18);
    uint64_t decl = r.u64(vb + 0x38);
    if (!r.valid(decl, 52 * 6) || stride == 0 || vcount == 0 || vcount > 2000000) return false;
    const char* v = r.at(vdata, qint64(stride) * vcount);
    if (!v) return false;
    auto elem = [&](int slot, uint32_t& off, uint32_t& estride, uint8_t& fmt) {
        off = r.u32(decl + uint64_t(slot) * 4);
        estride = r.u8(decl + 208 + uint64_t(slot));
        fmt = r.u8(decl + 260 + uint64_t(slot));
        if (!estride) estride = stride;
        return fmt != 0 && qint64(off) + qint64(estride) * (vcount - 1) + 4 <= qint64(stride) * vcount;
    };
    uint32_t pOff, pStr, nOff, nStr, uOff, uStr;
    uint8_t pFmt, nFmt, uFmt;
    if (!elem(0, pOff, pStr, pFmt)) return false;      // POSITION
    bool hasN = elem(4, nOff, nStr, nFmt);              // NORMAL
    bool hasUv = elem(28, uOff, uStr, uFmt);            // TEXCOORD0
    m.positions.resize(size_t(vcount) * 3);
    if (hasN) m.normals.resize(size_t(vcount) * 3);
    if (hasUv) m.uvs.resize(size_t(vcount) * 2);
    for (uint32_t i = 0; i < vcount; ++i) {
        float t[3] = {0, 0, 0};
        if (readGen9Element(v + pOff + size_t(pStr) * i, pFmt, t) < 3) return false;
        std::copy(t, t + 3, &m.positions[i * 3]);
        if (hasN) {
            float n[3] = {0, 0, 1};
            readGen9Element(v + nOff + size_t(nStr) * i, nFmt, n);
            std::copy(n, n + 3, &m.normals[i * 3]);
        }
        if (hasUv) {
            float uv[3] = {0, 0, 0};
            readGen9Element(v + uOff + size_t(uStr) * i, uFmt, uv);
            m.uvs[i * 2] = uv[0];
            m.uvs[i * 2 + 1] = uv[1];
        }
    }
    uint32_t icount = r.u32(ib + 0x08);
    uint64_t idata = r.u64(ib + 0x18);
    const char* ip = r.at(idata, qint64(icount) * 2);
    if (!ip) return false;
    m.indices.reserve(icount);
    for (uint32_t i = 0; i + 2 < icount; i += 3) {
        uint16_t a = rd16(ip + i * 2), b = rd16(ip + i * 2 + 2), c = rd16(ip + i * 2 + 4);
        if (a >= vcount || b >= vcount || c >= vcount) continue;
        m.indices.push_back(a);
        m.indices.push_back(b);
        m.indices.push_back(c);
    }
    return true;
}

bool readGeometry(const ResourceData& r, uint64_t gp, Mesh& m)
{
    if (!r.valid(gp, 0x98)) return false;
    uint64_t vb = r.u64(gp + 0x18), ib = r.u64(gp + 0x38);
    if (!r.valid(vb, 0x38) || !r.valid(ib, 0x18)) return false;
    uint16_t stride = r.u16(vb + 0x08);
    uint64_t vdata = r.u64(vb + 0x10);
    uint32_t vcount = r.u32(vb + 0x18);
    uint64_t decl = r.u64(vb + 0x30);
    if (!vdata) vdata = r.u64(vb + 0x20);
    if (!r.valid(decl, 16) || stride == 0 || vcount == 0 || vcount > 2000000) return false;
    uint32_t flags = r.u32(decl);
    uint64_t types = r.u64(decl + 8);
    const char* v = r.at(vdata, qint64(stride) * vcount);
    if (!v) return false;
    int offPos = -1, offNrm = -1, offUv = -1, tPos = 0, tNrm = 0, tUv = 0;
    int off = 0;
    for (int s = 0; s < 16; ++s) {
        if (!(flags & (1u << s))) continue;
        int t = int((types >> (s * 4)) & 0xF);
        if (s == 0) { offPos = off; tPos = t; }
        if (s == 3) { offNrm = off; tNrm = t; }
        if (s == 6) { offUv = off; tUv = t; }
        off += componentSize(t);
    }
    if (offPos < 0 || off > stride) return false;
    m.positions.resize(size_t(vcount) * 3);
    if (offNrm >= 0) m.normals.resize(size_t(vcount) * 3);
    if (offUv >= 0) m.uvs.resize(size_t(vcount) * 2);
    for (uint32_t i = 0; i < vcount; ++i) {
        const char* p = v + size_t(i) * stride;
        float t[3] = {0, 0, 0};
        readComponent(p + offPos, tPos, t);
        std::copy(t, t + 3, &m.positions[i * 3]);
        if (offNrm >= 0) {
            float n[3] = {0, 0, 1};
            readComponent(p + offNrm, tNrm, n);
            std::copy(n, n + 3, &m.normals[i * 3]);
        }
        if (offUv >= 0) {
            float uv[2] = {0, 0};
            float tmp[3] = {0, 0, 0};
            if (readComponent(p + offUv, tUv, tmp) >= 2) { uv[0] = tmp[0]; uv[1] = tmp[1]; }
            m.uvs[i * 2] = uv[0];
            m.uvs[i * 2 + 1] = uv[1];
        }
    }
    uint32_t icount = r.u32(ib + 0x08);
    uint64_t idata = r.u64(ib + 0x10);
    const char* ip = r.at(idata, qint64(icount) * 2);
    if (!ip) return false;
    m.indices.reserve(icount);
    for (uint32_t i = 0; i + 2 < icount; i += 3) {
        uint16_t a = rd16(ip + i * 2), b = rd16(ip + i * 2 + 2), c = rd16(ip + i * 2 + 4);
        if (a >= vcount || b >= vcount || c >= vcount) continue;
        m.indices.push_back(a);
        m.indices.push_back(b);
        m.indices.push_back(c);
    }
    return true;
}

bool readDrawable(const ResourceData& r, uint64_t dp, DrawableModel3D& out, bool hasName, bool gen9)
{
    if (!r.valid(dp, 0xA8)) return false;
    for (int i = 0; i < 3; ++i) {
        out.bbMin[i] = r.f32(dp + 0x30 + uint64_t(i) * 4);
        out.bbMax[i] = r.f32(dp + 0x40 + uint64_t(i) * 4);
    }
    if (hasName && r.valid(dp, 0xB0)) out.name = r.cstr(r.u64(dp + 0xA8));
    uint64_t sg = r.u64(dp + 0x10);
    auto shaders = gen9 ? readShadersGen9(r, sg) : readShaders(r, sg);
    if (r.valid(sg, 0x10)) {
        uint64_t td = r.u64(sg + 0x08);
        if (td && r.valid(td, 0x40)) out.hasEmbeddedTextures = out.embeddedTextures.parseAt(r, td, nullptr, gen9);
    }
    static const char* lodNames[] = {"High", "Medium", "Low", "Very low"};
    for (int lod = 0; lod < 4; ++lod) {
        uint64_t list = r.u64(dp + 0x50 + uint64_t(lod) * 8);
        if (!r.valid(list, 0x10)) continue;
        uint64_t models = r.u64(list);
        uint16_t mcount = r.u16(list + 8);
        if (mcount == 0 || mcount > 1024 || !r.valid(models, qint64(mcount) * 8)) continue;
        for (int mi = 0; mi < mcount; ++mi) {
            uint64_t mp = r.u64(models + uint64_t(mi) * 8);
            if (!r.valid(mp, 0x30)) continue;
            uint64_t geoms = r.u64(mp + 0x08);
            uint16_t gcount = r.u16(mp + 0x10);
            uint64_t smap = r.u64(mp + 0x20);
            if (gcount > 4096 || !r.valid(geoms, qint64(gcount) * 8)) continue;
            for (int gi = 0; gi < gcount; ++gi) {
                Mesh m;
                uint64_t gptr = r.u64(geoms + uint64_t(gi) * 8);
                if (!(gen9 ? readGeometryGen9(r, gptr, m) : readGeometry(r, gptr, m))) continue;
                if (r.valid(smap, qint64(gcount) * 2)) m.shaderIndex = r.u16(smap + uint64_t(gi) * 2);
                if (m.shaderIndex >= 0 && size_t(m.shaderIndex) < shaders.size()) m.diffuseTexture = shaders[size_t(m.shaderIndex)].diffuse;
                out.meshes.push_back(std::move(m));
            }
        }
        if (!out.meshes.empty()) { out.lodUsed = lodNames[lod]; break; }
    }
    // Texture-only drawables (e.g. vehicle livery fragments) are still valid.
    return !out.meshes.empty() || (out.hasEmbeddedTextures && !out.embeddedTextures.textures.empty());
}

} // namespace

bool loadDrawables(const QByteArray& rsc7, const QString& extIn, std::vector<DrawableModel3D>& out, QString* err)
{
    ResourceData r;
    if (!r.load(rsc7, err)) return false;
    QString ext = extIn.toLower();
    const uint64_t root = 0x50000000;
    // GTA V Enhanced resource versions (CodeWalker: Ydr 159/154, Ydd 159, Yft 171)
    const bool gen9 = r.version == 159 || r.version == 154 || r.version == 171;
    out.clear();
    if (ext == "ydr") {
        DrawableModel3D m;
        if (readDrawable(r, root, m, true, gen9)) out.push_back(std::move(m));
    } else if (ext == "ydd") {
        uint64_t hashes = r.u64(root + 0x20);
        uint64_t dptrs = r.u64(root + 0x30);
        uint16_t count = r.u16(root + 0x38);
        if (count < 4096 && r.valid(dptrs, qint64(count) * 8)) {
            for (int i = 0; i < count; ++i) {
                DrawableModel3D m;
                if (readDrawable(r, r.u64(dptrs + uint64_t(i) * 8), m, true, gen9)) {
                    if (m.name.isEmpty() && r.valid(hashes, qint64(count) * 4))
                        m.name = QStringLiteral("0x%1").arg(r.u32(hashes + uint64_t(i) * 4), 8, 16, QChar('0'));
                    out.push_back(std::move(m));
                }
            }
        }
    } else if (ext == "yft") {
        uint64_t dp = r.u64(root + 0x30);
        DrawableModel3D m;
        if (readDrawable(r, dp, m, false, gen9)) out.push_back(std::move(m));
        // fragment name
        if (!out.empty() && r.valid(root, 0x60)) out[0].name = r.cstr(r.u64(root + 0x58));
        // Some fragments (e.g. livery mods) keep their geometry in the drawable array instead.
        uint64_t arr = r.u64(root + 0x38), names = r.u64(root + 0x40);
        uint32_t count = r.u32(root + 0x48);
        if (count > 0 && count < 1024 && r.valid(arr, qint64(count) * 8)) {
            for (uint32_t i = 0; i < count; ++i) {
                DrawableModel3D am;
                if (!readDrawable(r, r.u64(arr + uint64_t(i) * 8), am, false, gen9)) continue;
                if (r.valid(names, qint64(count) * 8)) am.name = r.cstr(r.u64(names + uint64_t(i) * 8));
                if (am.name.isEmpty()) am.name = QStringLiteral("array %1").arg(i);
                out.push_back(std::move(am));
            }
        }
    } else {
        if (err) *err = "not a drawable type: " + ext;
        return false;
    }
    if (out.empty()) {
        if (err) *err = r.graphics.isEmpty() ? QStringLiteral("This file contains no geometry (collision/physics data only)")
                                             : QStringLiteral("No readable geometry found (resource version %1)").arg(r.version);
        return false;
    }
    return true;
}

QByteArray exportObj(const DrawableModel3D& m, const QString& mtlName)
{
    QByteArray o;
    o += "# Exported by RpfStudio\n";
    if (!mtlName.isEmpty()) o += "mtllib " + mtlName.toUtf8() + "\n";
    uint32_t base = 1;
    int gi = 0;
    for (const Mesh& mesh : m.meshes) {
        o += "o mesh_" + QByteArray::number(gi++) + "\n";
        size_t vc = mesh.positions.size() / 3;
        for (size_t i = 0; i < vc; ++i)
            o += "v " + QByteArray::number(mesh.positions[i*3], 'f', 6) + " " + QByteArray::number(mesh.positions[i*3+1], 'f', 6) + " " +
                 QByteArray::number(mesh.positions[i*3+2], 'f', 6) + "\n";
        for (size_t i = 0; i < mesh.uvs.size() / 2; ++i)
            o += "vt " + QByteArray::number(mesh.uvs[i*2], 'f', 6) + " " + QByteArray::number(1.0f - mesh.uvs[i*2+1], 'f', 6) + "\n";
        for (size_t i = 0; i < mesh.normals.size() / 3; ++i)
            o += "vn " + QByteArray::number(mesh.normals[i*3], 'f', 5) + " " + QByteArray::number(mesh.normals[i*3+1], 'f', 5) + " " +
                 QByteArray::number(mesh.normals[i*3+2], 'f', 5) + "\n";
        if (!mtlName.isEmpty() && !mesh.diffuseTexture.isEmpty()) o += "usemtl " + mesh.diffuseTexture.toUtf8() + "\n";
        bool hasUv = !mesh.uvs.empty(), hasN = !mesh.normals.empty();
        for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
            o += "f";
            for (int k = 0; k < 3; ++k) {
                QByteArray idx = QByteArray::number(mesh.indices[i + k] + base);
                o += " " + idx;
                if (hasUv || hasN) o += "/" + (hasUv ? idx : QByteArray());
                if (hasN) o += "/" + idx;
            }
            o += "\n";
        }
        base += uint32_t(vc);
    }
    return o;
}

QByteArray exportMtl(const DrawableModel3D& m)
{
    QByteArray o;
    QSet<QString> done;
    for (const Mesh& mesh : m.meshes) {
        if (mesh.diffuseTexture.isEmpty() || done.contains(mesh.diffuseTexture)) continue;
        done.insert(mesh.diffuseTexture);
        o += "newmtl " + mesh.diffuseTexture.toUtf8() + "\nKd 1 1 1\nmap_Kd " + mesh.diffuseTexture.toUtf8() + ".png\n\n";
    }
    return o;
}

} // namespace rs
