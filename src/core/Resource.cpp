#include "Resource.h"
#include "BinaryIO.h"
#include "Compression.h"
#include "Rpf.h"

namespace rs {

uint32_t resourceSizeFromFlags(uint32_t flags)
{
    uint32_t s0 = ((flags >> 27) & 0x1) << 0;
    uint32_t s1 = ((flags >> 26) & 0x1) << 1;
    uint32_t s2 = ((flags >> 25) & 0x1) << 2;
    uint32_t s3 = ((flags >> 24) & 0x1) << 3;
    uint32_t s4 = ((flags >> 17) & 0x7F) << 4;
    uint32_t s5 = ((flags >> 11) & 0x3F) << 5;
    uint32_t s6 = ((flags >> 7) & 0xF) << 6;
    uint32_t s7 = ((flags >> 5) & 0x3) << 7;
    uint32_t s8 = ((flags >> 4) & 0x1) << 8;
    uint32_t ss = flags & 0xF;
    uint64_t base = uint64_t(0x200) << ss;
    return uint32_t(base * (s0 + s1 + s2 + s3 + s4 + s5 + s6 + s7 + s8));
}

uint32_t resourceFlagsSinglePage(qint64 len, uint32_t ver, qint64* pageSize)
{
    // Pages come in five sizes (0x2000 << shift) * {1,2,4,8,16}. One page of the
    // base size (count in bits 17-23) holds up to 256 MB with shift 15.
    uint32_t f = (ver & 0xF) << 28;
    if (len <= 0) { if (pageSize) *pageSize = 0; return f; }
    uint32_t ss = 0;
    while ((qint64(0x2000) << ss) < len && ss < 15) ++ss;
    if (pageSize) *pageSize = qint64(0x2000) << ss;
    return f | ss | (1u << 17);
}

bool ResourceData::load(const QByteArray& d, QString* err)
{
    if (d.size() < 16 || rd32(d.constData()) != Rsc7Magic) {
        if (err) *err = "not an RSC7 resource";
        return false;
    }
    version = rd32(d.constData() + 4);
    systemFlags = rd32(d.constData() + 8);
    graphicsFlags = rd32(d.constData() + 12);
    qint64 ss = resourceSizeFromFlags(systemFlags), gs = resourceSizeFromFlags(graphicsFlags);
    QByteArray raw;
    if (!inflateRaw(d.mid(16), raw, ss + gs)) {
        if (err) *err = "resource decompression failed";
        return false;
    }
    if (raw.size() < ss) {
        if (err) *err = "resource data shorter than its page flags";
        return false;
    }
    system = raw.left(int(ss));
    graphics = raw.mid(int(ss));
    return true;
}

QByteArray ResourceData::build() const
{
    qint64 sl = 0, gl = 0;
    uint32_t sf = resourceFlagsSinglePage(system.size(), (version >> 4) & 0xF, &sl);
    uint32_t gf = resourceFlagsSinglePage(graphics.size(), version & 0xF, &gl);
    if (sl < system.size() || gl < graphics.size()) return {};  // > 256 MB segment
    QByteArray body = system;
    body.append(QByteArray(int(sl - system.size()), 0));
    body.append(graphics);
    body.append(QByteArray(int(gl - graphics.size()), 0));
    return makeRsc7Header(version, sf, gf) + deflateRaw(body);
}

const char* ResourceData::at(uint64_t ptr, qint64 len) const
{
    uint64_t base = ptr & 0xF0000000ull;
    uint64_t off = ptr & 0x0FFFFFFFull;
    if ((ptr >> 32) != 0) return nullptr;
    const QByteArray* seg = base == 0x50000000 ? &system : base == 0x60000000 ? &graphics : nullptr;
    if (!seg || len < 0 || qint64(off) + len > seg->size()) return nullptr;
    return seg->constData() + off;
}

bool ResourceData::valid(uint64_t ptr, qint64 len) const { return at(ptr, len) != nullptr; }
uint16_t ResourceData::u16(uint64_t p) const { auto d = at(p, 2); return d ? rd16(d) : 0; }
uint32_t ResourceData::u32(uint64_t p) const { auto d = at(p, 4); return d ? rd32(d) : 0; }
uint64_t ResourceData::u64(uint64_t p) const { auto d = at(p, 8); return d ? rd64(d) : 0; }
float ResourceData::f32(uint64_t p) const { auto d = at(p, 4); return d ? rdf(d) : 0.f; }

QByteArray ResourceData::bytes(uint64_t p, qint64 len) const
{
    auto d = at(p, len);
    return d ? QByteArray(d, int(len)) : QByteArray();
}

QString ResourceData::cstr(uint64_t p) const
{
    auto d = at(p, 1);
    if (!d) return {};
    const QByteArray* seg = (p & 0xF0000000ull) == 0x50000000 ? &system : &graphics;
    qint64 maxLen = seg->constData() + seg->size() - d;
    qint64 n = 0;
    while (n < maxLen && n < 1024 && d[n]) ++n;
    return QString::fromUtf8(d, int(n));
}

QString resourceTypeName(const QString& extIn)
{
    QString ext = extIn.toLower();
    if (ext == "ytd") return "Texture dictionary";
    if (ext == "ydr") return "Drawable";
    if (ext == "ydd") return "Drawable dictionary";
    if (ext == "yft") return "Fragment";
    if (ext == "ybn") return "Bounds";
    if (ext == "ycd") return "Clip dictionary";
    if (ext == "ymap") return "Map data";
    if (ext == "ytyp") return "Archetype definitions";
    if (ext == "ymt") return "Meta data";
    if (ext == "ynv") return "Navmesh";
    if (ext == "ynd") return "Path nodes";
    if (ext == "ysc") return "Script";
    if (ext == "ypt") return "Particle effects";
    if (ext == "yld") return "Cloth";
    if (ext == "yed") return "Expressions";
    if (ext == "awc") return "Audio wave container";
    if (ext == "gxt2") return "Text table";
    if (ext == "rpf") return "Archive";
    if (ext == "rel") return "Audio data";
    if (ext == "meta" || ext == "xml") return "XML";
    return ext.isEmpty() ? QString("File") : ext.toUpper() + " file";
}

} // namespace rs
