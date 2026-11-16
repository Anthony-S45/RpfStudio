#include "Ytd.h"
#include "BinaryIO.h"
#include "Crypto.h"

#include <algorithm>

namespace rs {

static constexpr int DictSize = 0x40;
static constexpr int TexStructSize = 0x90;

bool TextureDictionary::load(const QByteArray& rsc7, QString* err)
{
    ResourceData res;
    if (!res.load(rsc7, err)) return false;
    version = res.version;
    // 13 = legacy PC; everything else (5, and a few odd ones like 2) uses the Enhanced
    // layout, matching CodeWalker's behaviour in gen9 mode.
    return parseAt(res, 0x50000000, err, version != 13);
}

static uint32_t gen9ToLegacyFormat(uint8_t f)
{
    switch (f) {
    case 0x1C: case 0x1D: case 0x1B: return TexA8B8G8R8;
    case 0x57: case 0x5A: case 0x5B: return TexA8R8G8B8;
    case 0x58: case 0x5C: case 0x5D: return TexX8R8G8B8;
    case 0x41: return TexA8;
    case 0x3D: return TexL8;
    case 0x56: return TexA1R5G5B5;
    case 0x46: case 0x47: case 0x48: return TexDXT1;
    case 0x49: case 0x4A: case 0x4B: return TexDXT3;
    case 0x4C: case 0x4D: case 0x4E: return TexDXT5;
    case 0x4F: case 0x50: case 0x51: return TexATI1;
    case 0x52: case 0x53: case 0x54: return TexATI2;
    case 0x61: case 0x62: case 0x63: return TexBC7;
    default: return 0;
    }
}

static bool parseGen9Texture(const ResourceData& res, uint64_t tp, YtdTexture& t)
{
    // rage::grcTexture (gen9): 0x80 bytes, see CodeWalker TextureBase/Texture (IsGen9)
    if (!res.valid(tp, 0x50)) return false;
    t.rawStruct = res.bytes(tp, res.valid(tp, 0x80) ? 0x80 : 0x50);
    t.name = res.cstr(res.u64(tp + 0x28));
    t.image.width = res.u16(tp + 0x18);
    t.image.height = res.u16(tp + 0x1A);
    int depth = std::max<int>(1, res.u16(tp + 0x1C));
    t.gen9Format = res.u8(tp + 0x1F);
    t.image.format = gen9ToLegacyFormat(t.gen9Format);
    if (!t.image.format) t.image.format = 0xFF000000u | t.gen9Format;  // unknown: shown as such
    t.image.levels = std::max<int>(1, res.u8(tp + 0x22));
    uint64_t dataPtr = res.u64(tp + 0x38);
    if (texFormatKnown(t.image.format)) {
        qint64 len = texChainSize(t.image.format, t.image.width, t.image.height, t.image.levels) * depth;
        const char* base = res.at(dataPtr, 1);
        if (base) {
            const QByteArray& seg = (dataPtr & 0xF0000000ull) == 0x60000000 ? res.graphics : res.system;
            qint64 avail = seg.constData() + seg.size() - base;
            t.image.data = QByteArray(base, int(std::min(avail, len)));
        }
    }
    return true;
}

bool TextureDictionary::parseAt(const ResourceData& res, uint64_t ptr, QString* err, bool isGen9)
{
    textures.clear();
    gen9 = isGen9;
    if (!res.valid(ptr, DictSize)) { if (err) *err = "texture dictionary pointer out of range"; return false; }
    rawDict = res.bytes(ptr, DictSize);
    uint64_t texPtrs = res.u64(ptr + 0x30);
    uint16_t count = res.u16(ptr + 0x38);
    if (count > 4096 || (count && !res.valid(texPtrs, qint64(count) * 8))) {
        if (err) *err = "corrupt texture list";
        return false;
    }
    for (int i = 0; i < count; ++i) {
        uint64_t tp = res.u64(texPtrs + uint64_t(i) * 8);
        if (gen9) {
            YtdTexture t;
            if (parseGen9Texture(res, tp, t)) textures.push_back(std::move(t));
            continue;
        }
        if (!res.valid(tp, TexStructSize)) continue;
        YtdTexture t;
        t.rawStruct = res.bytes(tp, TexStructSize);
        t.name = res.cstr(res.u64(tp + 0x28));
        t.image.width = res.u16(tp + 0x50);
        t.image.height = res.u16(tp + 0x52);
        uint16_t stride = res.u16(tp + 0x56);
        t.image.format = res.u32(tp + 0x58);
        t.image.levels = std::max<int>(1, res.u8(tp + 0x5D));
        uint64_t dataPtr = res.u64(tp + 0x70);
        // Data size as the game lays it out (stride * height, quartering each level),
        // extended to the standard chain size when the buffer has it.
        qint64 len = 0, l = qint64(stride) * t.image.height;
        for (int m = 0; m < t.image.levels; ++m) { len += l; l /= 4; }
        qint64 stdLen = texFormatKnown(t.image.format) ? texChainSize(t.image.format, t.image.width, t.image.height, t.image.levels) : len;
        const char* base = res.at(dataPtr, 1);
        if (base) {
            const QByteArray& seg = (dataPtr & 0xF0000000ull) == 0x60000000 ? res.graphics : res.system;
            qint64 avail = seg.constData() + seg.size() - base;
            qint64 take = std::min(avail, std::max(len, stdLen));
            t.image.data = QByteArray(base, int(take));
        }
        textures.push_back(std::move(t));
    }
    return true;
}

int TextureDictionary::find(const QString& name) const
{
    for (size_t i = 0; i < textures.size(); ++i)
        if (textures[i].name.compare(name, Qt::CaseInsensitive) == 0) return int(i);
    return -1;
}

void TextureDictionary::setTexture(const QString& name, const TextureImage& img)
{
    int i = find(name);
    if (i >= 0) {
        textures[size_t(i)].image = img;
        return;
    }
    YtdTexture t;
    t.name = name;
    t.image = img;
    textures.push_back(std::move(t));
}

bool TextureDictionary::remove(const QString& name)
{
    int i = find(name);
    if (i < 0) return false;
    textures.erase(textures.begin() + i);
    return true;
}

static QByteArray defaultTexStruct()
{
    QByteArray s(TexStructSize, 0);
    wr32(s.data() + 0x04, 1);        // Unknown_4h
    wr16(s.data() + 0x30, 1);        // Unknown_30h
    wr16(s.data() + 0x32, 2);        // Unknown_32h
    return s;
}

QByteArray TextureDictionary::build(QString* err) const
{
    if (gen9) {
        if (err) *err = "Saving GTA V Enhanced (gen9) texture dictionaries is not supported yet";
        return {};
    }
    // Sort by name hash: the game binary-searches the hash list.
    struct Item { uint32_t hash; const YtdTexture* t; };
    std::vector<Item> items;
    for (auto& t : textures) items.push_back({joaatLower(t.name), &t});
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.hash < b.hash; });
    for (size_t i = 1; i < items.size(); ++i) {
        if (items[i].hash == items[i - 1].hash) {
            if (err) *err = "Duplicate texture name: " + items[i].t->name;
            return {};
        }
    }
    const uint32_t n = uint32_t(items.size());
    const uint64_t S = 0x50000000, G = 0x60000000;

    // ---- system segment layout
    qint64 off = DictSize;
    const qint64 pagesInfoOff = off;  off += 16 + 16;      // 1 system + 1 graphics page
    const qint64 hashesOff = alignUp(off, 16); off = hashesOff + n * 4;
    const qint64 ptrsOff = alignUp(off, 16);   off = ptrsOff + n * 8;
    const qint64 structsOff = alignUp(off, 16); off = structsOff + qint64(n) * TexStructSize;
    std::vector<qint64> nameOffs(n);
    QByteArray names;
    for (uint32_t i = 0; i < n; ++i) {
        nameOffs[i] = off + names.size();
        names += items[i].t->name.toUtf8();
        names += '\0';
    }
    off += names.size();
    QByteArray sys(int(alignUp(off, 16)), 0);

    // ---- graphics segment
    QByteArray gfx;
    std::vector<qint64> dataOffs(n);
    for (uint32_t i = 0; i < n; ++i) {
        gfx.append(QByteArray(int(alignUp(gfx.size(), 16) - gfx.size()), 0));
        dataOffs[i] = gfx.size();
        gfx += items[i].t->image.data;
    }

    // dictionary
    QByteArray dict = rawDict.size() == DictSize ? rawDict : QByteArray(DictSize, 0);
    if (rawDict.size() != DictSize) { wr32(dict.data() + 4, 1); wr32(dict.data() + 0x18, 1); }
    char* d = dict.data();
    wr64(d + 0x08, S + uint64_t(pagesInfoOff));
    wr64(d + 0x20, n ? S + uint64_t(hashesOff) : 0);
    wr16(d + 0x28, uint16_t(n)); wr16(d + 0x2A, uint16_t(n)); wr32(d + 0x2C, 0);
    wr64(d + 0x30, n ? S + uint64_t(ptrsOff) : 0);
    wr16(d + 0x38, uint16_t(n)); wr16(d + 0x3A, uint16_t(n)); wr32(d + 0x3C, 0);
    std::memcpy(sys.data(), dict.constData(), DictSize);

    // pages info
    char* pi = sys.data() + pagesInfoOff;
    wr32(pi, 0); wr32(pi + 4, 0);
    pi[8] = 1; pi[9] = gfx.isEmpty() ? 0 : 1;

    const YtdTexture* tmpl = nullptr;
    for (auto& t : textures) if (t.rawStruct.size() == TexStructSize) { tmpl = &t; break; }

    for (uint32_t i = 0; i < n; ++i) {
        const YtdTexture& t = *items[i].t;
        wr32(sys.data() + hashesOff + i * 4, items[i].hash);
        wr64(sys.data() + ptrsOff + i * 8, S + uint64_t(structsOff + qint64(i) * TexStructSize));
        QByteArray ts = t.rawStruct.size() == TexStructSize ? t.rawStruct
                        : (tmpl ? tmpl->rawStruct : defaultTexStruct());
        char* p = ts.data();
        wr64(p + 0x28, S + uint64_t(nameOffs[i]));
        wr16(p + 0x50, uint16_t(t.image.width));
        wr16(p + 0x52, uint16_t(t.image.height));
        wr16(p + 0x54, 1);
        wr16(p + 0x56, uint16_t(texRowPitch(t.image.format, t.image.width)));
        wr32(p + 0x58, t.image.format);
        p[0x5D] = char(uint8_t(t.image.levels));
        wr64(p + 0x70, G + uint64_t(dataOffs[i]));
        std::memcpy(sys.data() + structsOff + qint64(i) * TexStructSize, ts.constData(), TexStructSize);
    }
    if (!names.isEmpty()) std::memcpy(sys.data() + nameOffs[0], names.constData(), size_t(names.size()));

    ResourceData res;
    res.version = version ? version : 13;
    res.system = sys;
    res.graphics = gfx;
    QByteArray out = res.build();
    if (out.isEmpty() && err) *err = "Texture dictionary too large (over 256 MB)";
    return out;
}

} // namespace rs
