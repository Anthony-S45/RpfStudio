#include "Texture.h"
#include "BinaryIO.h"

#define BCDEC_IMPLEMENTATION
#define BCDEC_STATIC
#include "../../third_party/bcdec/bcdec.h"

#include <algorithm>
#include <cmath>

namespace rs {

QString texFormatName(uint32_t f)
{
    switch (f) {
    case TexA8R8G8B8: return "A8R8G8B8";
    case TexX8R8G8B8: return "X8R8G8B8";
    case TexA1R5G5B5: return "A1R5G5B5";
    case TexA8: return "A8";
    case TexA8B8G8R8: return "A8B8G8R8";
    case TexL8: return "L8";
    case TexDXT1: return "DXT1 (BC1)";
    case TexDXT3: return "DXT3 (BC2)";
    case TexDXT5: return "DXT5 (BC3)";
    case TexATI1: return "ATI1 (BC4)";
    case TexATI2: return "ATI2 (BC5)";
    case TexBC7: return "BC7";
    default: return QStringLiteral("Unknown (0x%1)").arg(f, 0, 16);
    }
}

bool texFormatKnown(uint32_t f) { return texBitsPerPixel(f) > 0; }

bool texIsBlock(uint32_t f)
{
    return f == TexDXT1 || f == TexDXT3 || f == TexDXT5 || f == TexATI1 || f == TexATI2 || f == TexBC7;
}

int texBlockBytes(uint32_t f) { return (f == TexDXT1 || f == TexATI1) ? 8 : 16; }

int texBitsPerPixel(uint32_t f)
{
    switch (f) {
    case TexA8R8G8B8: case TexX8R8G8B8: case TexA8B8G8R8: return 32;
    case TexA1R5G5B5: return 16;
    case TexA8: case TexL8: return 8;
    case TexDXT1: case TexATI1: return 4;
    case TexDXT3: case TexDXT5: case TexATI2: case TexBC7: return 8;
    default: return 0;
    }
}

qint64 texRowPitch(uint32_t f, int w)
{
    if (texIsBlock(f)) return qint64(std::max(1, (w + 3) / 4)) * texBlockBytes(f);
    return (qint64(w) * texBitsPerPixel(f) + 7) / 8;
}

qint64 texLevelSize(uint32_t f, int w, int h)
{
    if (texIsBlock(f)) return texRowPitch(f, w) * std::max(1, (h + 3) / 4);
    return texRowPitch(f, w) * h;
}

qint64 texChainSize(uint32_t f, int w, int h, int levels)
{
    qint64 s = 0;
    for (int i = 0; i < levels; ++i) {
        s += texLevelSize(f, std::max(1, w >> i), std::max(1, h >> i));
    }
    return s;
}

QImage decodeTextureLevel(uint32_t fmt, int w, int h, const char* data, qint64 size)
{
    if (w <= 0 || h <= 0 || !texFormatKnown(fmt) || size < texLevelSize(fmt, w, h)) return {};
    QImage img(w, h, QImage::Format_RGBA8888);
    const uint8_t* src = reinterpret_cast<const uint8_t*>(data);
    if (texIsBlock(fmt)) {
        const int bw = (w + 3) / 4, bh = (h + 3) / 4, bb = texBlockBytes(fmt);
        uint8_t px[4 * 4 * 4];
        uint8_t ch[4 * 4 * 2];
        for (int by = 0; by < bh; ++by) {
            for (int bx = 0; bx < bw; ++bx) {
                const uint8_t* blk = src + (qint64(by) * bw + bx) * bb;
                switch (fmt) {
                case TexDXT1: bcdec_bc1(blk, px, 16); break;
                case TexDXT3: bcdec_bc2(blk, px, 16); break;
                case TexDXT5: bcdec_bc3(blk, px, 16); break;
                case TexBC7: bcdec_bc7(blk, px, 16); break;
                case TexATI1:
                    bcdec_bc4(blk, ch, 4);
                    for (int i = 0; i < 16; ++i) { px[i*4] = px[i*4+1] = px[i*4+2] = ch[i]; px[i*4+3] = 255; }
                    break;
                case TexATI2:
                    bcdec_bc5(blk, ch, 8);
                    for (int i = 0; i < 16; ++i) {
                        // normal maps: reconstruct Z for a pleasant preview
                        float x = ch[i*2] / 127.5f - 1.f, y = ch[i*2+1] / 127.5f - 1.f;
                        float z = std::sqrt(std::max(0.f, 1.f - x * x - y * y));
                        px[i*4] = ch[i*2]; px[i*4+1] = ch[i*2+1]; px[i*4+2] = uint8_t(std::lround((z * 0.5f + 0.5f) * 255)); px[i*4+3] = 255;
                    }
                    break;
                }
                for (int y = 0; y < 4; ++y) {
                    int py = by * 4 + y;
                    if (py >= h) break;
                    uint8_t* line = img.scanLine(py);
                    for (int x = 0; x < 4; ++x) {
                        int pxx = bx * 4 + x;
                        if (pxx >= w) break;
                        std::memcpy(line + pxx * 4, px + (y * 4 + x) * 4, 4);
                    }
                }
            }
        }
        return img;
    }
    const qint64 pitch = texRowPitch(fmt, w);
    for (int y = 0; y < h; ++y) {
        const uint8_t* s = src + pitch * y;
        uint8_t* d = img.scanLine(y);
        for (int x = 0; x < w; ++x, d += 4) {
            switch (fmt) {
            case TexA8R8G8B8: d[0] = s[x*4+2]; d[1] = s[x*4+1]; d[2] = s[x*4]; d[3] = s[x*4+3]; break;
            case TexX8R8G8B8: d[0] = s[x*4+2]; d[1] = s[x*4+1]; d[2] = s[x*4]; d[3] = 255; break;
            case TexA8B8G8R8: std::memcpy(d, s + x * 4, 4); break;
            case TexA1R5G5B5: {
                uint16_t v = rd16(s + x * 2);
                d[0] = uint8_t(((v >> 10) & 31) * 255 / 31);
                d[1] = uint8_t(((v >> 5) & 31) * 255 / 31);
                d[2] = uint8_t((v & 31) * 255 / 31);
                d[3] = (v & 0x8000) ? 255 : 0;
                break;
            }
            case TexA8: d[0] = d[1] = d[2] = 255; d[3] = s[x]; break;
            case TexL8: d[0] = d[1] = d[2] = s[x]; d[3] = 255; break;
            }
        }
    }
    return img;
}

QImage decodeTexture(const TextureImage& t, int level)
{
    qint64 off = 0;
    for (int i = 0; i < level; ++i) off += texLevelSize(t.format, std::max(1, t.width >> i), std::max(1, t.height >> i));
    int w = std::max(1, t.width >> level), h = std::max(1, t.height >> level);
    if (off >= t.data.size()) return {};
    return decodeTextureLevel(t.format, w, h, t.data.constData() + off, t.data.size() - off);
}

// ------------------------------------------------------------------ DDS

namespace {
constexpr uint32_t FourCC(char a, char b, char c, char d)
{
    return uint32_t(uint8_t(a)) | uint32_t(uint8_t(b)) << 8 | uint32_t(uint8_t(c)) << 16 | uint32_t(uint8_t(d)) << 24;
}
}

QByteArray writeDds(const TextureImage& t)
{
    Writer w;
    w.u32(FourCC('D', 'D', 'S', ' '));
    w.u32(124);
    uint32_t flags = 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000;
    flags |= texIsBlock(t.format) ? 0x80000 : 0x8;
    w.u32(flags);
    w.u32(uint32_t(t.height));
    w.u32(uint32_t(t.width));
    w.u32(uint32_t(texIsBlock(t.format) ? texLevelSize(t.format, t.width, t.height) : texRowPitch(t.format, t.width)));
    w.u32(0);
    w.u32(uint32_t(std::max(1, t.levels)));
    for (int i = 0; i < 11; ++i) w.u32(0);
    // pixel format
    w.u32(32);
    bool dx10 = t.format == TexBC7;
    switch (t.format) {
    case TexDXT1: case TexDXT3: case TexDXT5: case TexATI1: case TexATI2:
        w.u32(0x4); w.u32(t.format); w.u32(0); w.u32(0); w.u32(0); w.u32(0); w.u32(0); break;
    case TexBC7:
        w.u32(0x4); w.u32(FourCC('D', 'X', '1', '0')); w.u32(0); w.u32(0); w.u32(0); w.u32(0); w.u32(0); break;
    case TexA8R8G8B8:
        w.u32(0x41); w.u32(0); w.u32(32); w.u32(0x00FF0000); w.u32(0x0000FF00); w.u32(0x000000FF); w.u32(0xFF000000); break;
    case TexX8R8G8B8:
        w.u32(0x40); w.u32(0); w.u32(32); w.u32(0x00FF0000); w.u32(0x0000FF00); w.u32(0x000000FF); w.u32(0); break;
    case TexA8B8G8R8:
        w.u32(0x41); w.u32(0); w.u32(32); w.u32(0x000000FF); w.u32(0x0000FF00); w.u32(0x00FF0000); w.u32(0xFF000000); break;
    case TexA1R5G5B5:
        w.u32(0x41); w.u32(0); w.u32(16); w.u32(0x7C00); w.u32(0x03E0); w.u32(0x001F); w.u32(0x8000); break;
    case TexA8:
        w.u32(0x2); w.u32(0); w.u32(8); w.u32(0); w.u32(0); w.u32(0); w.u32(0xFF); break;
    case TexL8:
        w.u32(0x20000); w.u32(0); w.u32(8); w.u32(0xFF); w.u32(0); w.u32(0); w.u32(0); break;
    default:
        w.u32(0); w.u32(0); w.u32(0); w.u32(0); w.u32(0); w.u32(0); w.u32(0); break;
    }
    w.u32(0x1000 | (t.levels > 1 ? 0x400008 : 0));
    w.u32(0); w.u32(0); w.u32(0); w.u32(0);
    if (dx10) { w.u32(98); w.u32(3); w.u32(0); w.u32(1); w.u32(0); }
    // Pad the mip chain to the size DDS readers expect.
    QByteArray body = t.data;
    qint64 want = texChainSize(t.format, t.width, t.height, std::max(1, t.levels));
    if (body.size() < want) body.append(QByteArray(int(want - body.size()), 0));
    else if (body.size() > want) body.truncate(int(want));
    w.bytes(body);
    return w.data;
}

bool readDds(const QByteArray& d, TextureImage& out, QString* err)
{
    auto fail = [&](const QString& m) { if (err) *err = m; return false; };
    if (d.size() < 128 || rd32(d.constData()) != FourCC('D', 'D', 'S', ' ')) return fail("not a DDS file");
    const char* h = d.constData() + 4;
    int height = int(rd32(h + 8)), width = int(rd32(h + 12));
    int mips = int(rd32(h + 24));
    const char* pf = h + 72;
    uint32_t pflags = rd32(pf + 4), fourcc = rd32(pf + 8), bits = rd32(pf + 12);
    uint32_t rm = rd32(pf + 16), gm = rd32(pf + 20), bm = rd32(pf + 24), am = rd32(pf + 28);
    qint64 dataOff = 128;
    uint32_t fmt = 0;
    if (pflags & 0x4) {
        if (fourcc == FourCC('D', 'X', '1', '0')) {
            if (d.size() < 148) return fail("truncated DX10 header");
            uint32_t dxgi = rd32(d.constData() + 128);
            dataOff = 148;
            switch (dxgi) {
            case 70: case 71: case 72: fmt = TexDXT1; break;
            case 73: case 74: case 75: fmt = TexDXT3; break;
            case 76: case 77: case 78: fmt = TexDXT5; break;
            case 79: case 80: fmt = TexATI1; break;
            case 82: case 83: fmt = TexATI2; break;
            case 97: case 98: case 99: fmt = TexBC7; break;
            case 87: case 91: fmt = TexA8R8G8B8; break;
            case 88: case 93: fmt = TexX8R8G8B8; break;
            case 27: case 28: case 29: fmt = TexA8B8G8R8; break;
            case 65: fmt = TexA8; break;
            case 61: fmt = TexL8; break;
            case 86: fmt = TexA1R5G5B5; break;
            default: return fail(QStringLiteral("unsupported DXGI format %1").arg(dxgi));
            }
        } else if (fourcc == TexDXT1 || fourcc == TexDXT3 || fourcc == TexDXT5 || fourcc == TexATI1 || fourcc == TexATI2) {
            fmt = fourcc;
        } else if (fourcc == FourCC('B', 'C', '4', 'U')) {
            fmt = TexATI1;
        } else if (fourcc == FourCC('B', 'C', '5', 'U')) {
            fmt = TexATI2;
        } else if (fourcc == FourCC('D', 'X', 'T', '2')) {
            fmt = TexDXT3;
        } else if (fourcc == FourCC('D', 'X', 'T', '4')) {
            fmt = TexDXT5;
        } else {
            return fail("unsupported DDS fourCC");
        }
    } else if (bits == 32 && rm == 0x00FF0000 && gm == 0xFF00 && bm == 0xFF) {
        fmt = (am == 0xFF000000 && (pflags & 0x1)) ? TexA8R8G8B8 : TexX8R8G8B8;
    } else if (bits == 32 && rm == 0xFF && gm == 0xFF00 && bm == 0xFF0000) {
        fmt = TexA8B8G8R8;
    } else if (bits == 16 && rm == 0x7C00) {
        fmt = TexA1R5G5B5;
    } else if (bits == 8 && (pflags & 0x2)) {
        fmt = TexA8;
    } else if (bits == 8) {
        fmt = TexL8;
    } else {
        return fail("unsupported uncompressed DDS layout");
    }
    if (width <= 0 || height <= 0 || width > 16384 || height > 16384) return fail("bad DDS dimensions");
    if (mips < 1) mips = 1;
    // Clamp mips to what is present.
    int maxLevels = 1;
    while ((width >> maxLevels) > 0 || (height >> maxLevels) > 0) ++maxLevels;
    mips = std::min(mips, maxLevels);
    qint64 avail = d.size() - dataOff;
    while (mips > 1 && texChainSize(fmt, width, height, mips) > avail) --mips;
    qint64 need = texChainSize(fmt, width, height, mips);
    if (avail < need) return fail("DDS file is truncated");
    out.format = fmt;
    out.width = width;
    out.height = height;
    out.levels = mips;
    out.data = d.mid(int(dataOff), int(need));
    return true;
}

// ------------------------------------------------------------------ encoding

namespace {

inline uint16_t to565(int r, int g, int b)
{
    return uint16_t(((r * 31 + 127) / 255) << 11 | ((g * 63 + 127) / 255) << 5 | ((b * 31 + 127) / 255));
}
inline void from565(uint16_t c, int& r, int& g, int& b)
{
    r = ((c >> 11) & 31) * 255 / 31;
    g = ((c >> 5) & 63) * 255 / 63;
    b = (c & 31) * 255 / 31;
}

void encodeColorBlock(const uint8_t* px /*16 rgba*/, uint8_t* out, bool dxt1Alpha)
{
    int mn[3] = {255, 255, 255}, mx[3] = {0, 0, 0};
    bool anyTransparent = false;
    for (int i = 0; i < 16; ++i) {
        if (dxt1Alpha && px[i * 4 + 3] < 128) { anyTransparent = true; continue; }
        for (int c = 0; c < 3; ++c) { mn[c] = std::min(mn[c], int(px[i*4+c])); mx[c] = std::max(mx[c], int(px[i*4+c])); }
    }
    if (mn[0] > mx[0]) { mn[0] = mn[1] = mn[2] = mx[0] = mx[1] = mx[2] = 0; }
    // inset bounding box slightly
    for (int c = 0; c < 3; ++c) { int inset = (mx[c] - mn[c]) / 16; mn[c] += inset; mx[c] -= inset; }
    uint16_t c0 = to565(mx[0], mx[1], mx[2]), c1 = to565(mn[0], mn[1], mn[2]);
    bool threeColor = dxt1Alpha && anyTransparent;
    if (threeColor ? c0 > c1 : c0 < c1) std::swap(c0, c1);
    if (!threeColor && c0 == c1) { if (c1 > 0) --c1; else ++c0; }
    int pal[4][3];
    from565(c0, pal[0][0], pal[0][1], pal[0][2]);
    from565(c1, pal[1][0], pal[1][1], pal[1][2]);
    for (int c = 0; c < 3; ++c) {
        if (c0 > c1) {
            pal[2][c] = (2 * pal[0][c] + pal[1][c]) / 3;
            pal[3][c] = (pal[0][c] + 2 * pal[1][c]) / 3;
        } else {
            pal[2][c] = (pal[0][c] + pal[1][c]) / 2;
            pal[3][c] = 0;
        }
    }
    uint32_t idx = 0;
    for (int i = 0; i < 16; ++i) {
        int best = 0, bestd = INT32_MAX;
        if (threeColor && px[i * 4 + 3] < 128) { best = 3; }
        else {
            int n = (c0 > c1) ? 4 : 3;
            for (int p = 0; p < n; ++p) {
                int dr = px[i*4] - pal[p][0], dg = px[i*4+1] - pal[p][1], db = px[i*4+2] - pal[p][2];
                int dd = dr * dr + dg * dg + db * db;
                if (dd < bestd) { bestd = dd; best = p; }
            }
        }
        idx |= uint32_t(best) << (i * 2);
    }
    wr16(out, c0);
    wr16(out + 2, c1);
    wr32(out + 4, idx);
}

void encodeAlphaBlock(const uint8_t* px, uint8_t* out)
{
    int mn = 255, mx = 0;
    for (int i = 0; i < 16; ++i) { mn = std::min(mn, int(px[i*4+3])); mx = std::max(mx, int(px[i*4+3])); }
    out[0] = uint8_t(mx);
    out[1] = uint8_t(mn);
    int pal[8];
    pal[0] = mx; pal[1] = mn;
    if (mx > mn) for (int i = 1; i < 7; ++i) pal[i + 1] = ((7 - i) * mx + i * mn) / 7;
    else for (int i = 2; i < 8; ++i) pal[i] = mn;
    uint64_t bits = 0;
    for (int i = 0; i < 16; ++i) {
        int a = px[i*4+3], best = 0, bd = 1 << 30;
        for (int p = 0; p < 8; ++p) { int d = std::abs(a - pal[p]); if (d < bd) { bd = d; best = p; } }
        bits |= uint64_t(best) << (3 * i);
    }
    for (int i = 0; i < 6; ++i) out[2 + i] = uint8_t(bits >> (8 * i));
}

QByteArray encodeLevel(const QImage& src, uint32_t fmt)
{
    QImage img = src.convertToFormat(QImage::Format_RGBA8888);
    const int w = img.width(), h = img.height();
    QByteArray out;
    if (fmt == TexA8R8G8B8 || fmt == TexA8B8G8R8) {
        out.resize(w * h * 4);
        uint8_t* d = reinterpret_cast<uint8_t*>(out.data());
        for (int y = 0; y < h; ++y) {
            const uint8_t* s = img.constScanLine(y);
            for (int x = 0; x < w; ++x, d += 4, s += 4) {
                if (fmt == TexA8R8G8B8) { d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = s[3]; }
                else std::memcpy(d, s, 4);
            }
        }
        return out;
    }
    const int bw = std::max(1, (w + 3) / 4), bh = std::max(1, (h + 3) / 4);
    const int bb = fmt == TexDXT1 ? 8 : 16;
    out.resize(bw * bh * bb);
    uint8_t px[64];
    for (int by = 0; by < bh; ++by) {
        for (int bx = 0; bx < bw; ++bx) {
            for (int y = 0; y < 4; ++y)
                for (int x = 0; x < 4; ++x) {
                    int sx = std::min(w - 1, bx * 4 + x), sy = std::min(h - 1, by * 4 + y);
                    std::memcpy(px + (y * 4 + x) * 4, img.constScanLine(sy) + sx * 4, 4);
                }
            uint8_t* o = reinterpret_cast<uint8_t*>(out.data()) + (by * bw + bx) * bb;
            if (fmt == TexDXT1) encodeColorBlock(px, o, true);
            else { encodeAlphaBlock(px, o); encodeColorBlock(px, o + 8, false); }
        }
    }
    return out;
}

} // namespace

TextureImage encodeImage(const QImage& img, uint32_t fmt, bool mipmaps)
{
    if (fmt != TexDXT1 && fmt != TexDXT5 && fmt != TexA8R8G8B8 && fmt != TexA8B8G8R8) fmt = TexDXT5;
    TextureImage t;
    t.format = fmt;
    t.width = img.width();
    t.height = img.height();
    t.levels = 1;
    QImage level = img.convertToFormat(QImage::Format_RGBA8888);
    t.data = encodeLevel(level, fmt);
    if (mipmaps) {
        // Block formats stop at 4x4, like the game's own textures.
        int minSize = texIsBlock(fmt) ? 4 : 1;
        while (level.width() > minSize || level.height() > minSize) {
            int nw = std::max(1, level.width() / 2), nh = std::max(1, level.height() / 2);
            if (texIsBlock(fmt) && (nw < 4 || nh < 4)) break;
            level = level.scaled(nw, nh, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
            t.data += encodeLevel(level, fmt);
            ++t.levels;
        }
    }
    return t;
}

} // namespace rs
