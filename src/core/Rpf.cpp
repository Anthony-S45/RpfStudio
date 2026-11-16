#include "Rpf.h"
#include "BinaryIO.h"
#include "Compression.h"
#include "Crypto.h"

#include <QFileInfo>
#include <deque>

namespace rs {

QString encryptionName(uint32_t enc)
{
    switch (enc) {
    case RpfEncNone: return QStringLiteral("None");
    case RpfEncOpen: return QStringLiteral("OPEN (unencrypted)");
    case RpfEncAes: return QStringLiteral("AES");
    case RpfEncNg: return QStringLiteral("NG");
    default: return QStringLiteral("Unknown (0x%1)").arg(enc, 8, 16, QChar('0'));
    }
}

std::shared_ptr<FileSource> FileSource::open(const QString& path, QString* err)
{
    auto s = std::make_shared<FileSource>();
    s->m_file.setFileName(path);
    if (!s->m_file.open(QIODevice::ReadOnly)) {
        if (err) *err = QStringLiteral("Cannot open %1: %2").arg(path, s->m_file.errorString());
        return nullptr;
    }
    s->m_path = QFileInfo(path).absoluteFilePath();
    s->m_size = s->m_file.size();
    return s;
}

QByteArray FileSource::read(qint64 offset, qint64 length)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    if (offset < 0 || length < 0 || offset + length > m_size) return {};
    if (!m_file.seek(offset)) return {};
    return m_file.read(length);
}

QByteArray makeRsc7Header(uint32_t version, uint32_t sysFlags, uint32_t gfxFlags)
{
    Writer w;
    w.u32(Rsc7Magic);
    w.u32(version);
    w.u32(sysFlags);
    w.u32(gfxFlags);
    return w.data;
}

std::shared_ptr<RpfArchive> RpfArchive::openFile(const QString& path, QString* err)
{
    auto src = FileSource::open(path, err);
    if (!src) return nullptr;
    auto a = std::shared_ptr<RpfArchive>(new RpfArchive());
    a->m_src = src;
    a->m_name = QFileInfo(path).fileName();
    a->m_base = 0;
    a->m_size = src->size();
    if (!a->parse(err)) return nullptr;
    return a;
}

std::shared_ptr<RpfArchive> RpfArchive::openNested(const std::shared_ptr<RpfArchive>& parent, int idx, QString* err)
{
    const RpfEntry& e = parent->entry(idx);
    if (e.type != RpfEntry::Binary) {
        if (err) *err = QStringLiteral("%1 is not an archive entry").arg(e.name);
        return nullptr;
    }
    if (e.isCompressed() || e.encrypted) {
        if (err) *err = QStringLiteral("Nested archive %1 is compressed/encrypted; not supported").arg(e.name);
        return nullptr;
    }
    auto a = std::shared_ptr<RpfArchive>(new RpfArchive());
    a->m_src = parent->m_src;
    a->m_parent = parent;
    a->m_parentEntry = idx;
    a->m_name = e.name;
    a->m_internalPath = parent->m_internalPath.isEmpty() ? parent->entryPath(idx)
                                                         : parent->m_internalPath + "/" + parent->entryPath(idx);
    a->m_base = parent->m_base + qint64(e.offsetBlocks) * 512;
    a->m_size = e.storedSize();
    if (!a->parse(err)) return nullptr;
    return a;
}

std::shared_ptr<RpfArchive> RpfArchive::topLevel()
{
    auto a = shared_from_this();
    while (a->m_parent) a = a->m_parent;
    return a;
}

QString RpfArchive::displayPath() const
{
    return m_internalPath.isEmpty() ? physicalPath() : physicalPath() + "/" + m_internalPath;
}

bool RpfArchive::parse(QString* err)
{
    auto fail = [&](const QString& m) { if (err) *err = m_name + ": " + m; return false; };
    QByteArray hdr = m_src->read(m_base, 16);
    if (hdr.size() != 16) return fail("file too small");
    uint32_t magic = rd32(hdr.constData());
    uint32_t count = rd32(hdr.constData() + 4);
    uint32_t namesLen = rd32(hdr.constData() + 8);
    m_encryption = rd32(hdr.constData() + 12);
    if (magic != RpfMagic) return fail("not an RPF7 archive");
    if (count == 0 || count > 0x100000 || namesLen > 0x1000000) return fail("corrupt header");

    QByteArray entries = m_src->read(m_base + 16, qint64(count) * 16);
    QByteArray names = m_src->read(m_base + 16 + qint64(count) * 16, namesLen);
    if (entries.size() != int(count * 16) || names.size() != int(namesLen)) return fail("truncated header");

    const GameKeys& keys = GameKeys::global();
    switch (m_encryption) {
    case RpfEncNone:
    case RpfEncOpen:
        break;
    case RpfEncAes:
        if (!keys.hasAes()) return fail("archive is AES encrypted and no AES key is loaded");
        aesDecrypt(entries, keys.aesKey);
        aesDecrypt(names, keys.aesKey);
        break;
    default: // NG (and unknown values, which the game also treats as NG)
        if (!keys.hasNg()) return fail("archive is NG encrypted and no NG keys are loaded (see Settings > Keys)");
        ngDecrypt(entries, m_name, uint32_t(m_size), keys);
        ngDecrypt(names, m_name, uint32_t(m_size), keys);
        break;
    }

    m_entries.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
        const char* p = entries.constData() + i * 16;
        RpfEntry& e = m_entries[i];
        uint32_t h2 = rd32(p + 4);
        uint32_t nameOffset;
        if (h2 == 0x7FFFFF00) {
            e.type = RpfEntry::Directory;
            nameOffset = rd32(p);
            e.entriesIndex = rd32(p + 8);
            e.entriesCount = rd32(p + 12);
        } else if ((h2 & 0x80000000u) == 0) {
            e.type = RpfEntry::Binary;
            uint64_t v = rd64(p);
            nameOffset = uint32_t(v & 0xFFFF);
            e.sizeField = uint32_t((v >> 16) & 0xFFFFFF);
            e.offsetBlocks = uint32_t((v >> 40) & 0xFFFFFF);
            e.uncompressedSize = rd32(p + 8);
            uint32_t et = rd32(p + 12);
            if (et > 1) return fail(QStringLiteral("bad file entry %1 (wrong decryption key?)").arg(i));
            e.encrypted = et == 1;
        } else {
            e.type = RpfEntry::Resource;
            const uint8_t* b = reinterpret_cast<const uint8_t*>(p);
            nameOffset = rd16(p);
            e.sizeField = uint32_t(b[2]) | uint32_t(b[3]) << 8 | uint32_t(b[4]) << 16;
            e.offsetBlocks = (uint32_t(b[5]) | uint32_t(b[6]) << 8 | uint32_t(b[7]) << 16) & 0x7FFFFF;
            e.systemFlags = rd32(p + 8);
            e.graphicsFlags = rd32(p + 12);
        }
        if (nameOffset >= namesLen && !(i == 0 && namesLen == 0))
            return fail(QStringLiteral("bad name offset in entry %1 (wrong decryption key?)").arg(i));
        int end = names.indexOf('\0', int(nameOffset));
        if (end < 0) end = names.size();
        e.name = QString::fromUtf8(names.constData() + nameOffset, end - int(nameOffset)).left(256);
        if (e.type == RpfEntry::Resource) {
            e.encrypted = e.name.endsWith(QLatin1String(".ysc"), Qt::CaseInsensitive);
            if (e.sizeField == 0xFFFFFF) { // huge resource: real size is packed into its header
                QByteArray rh = m_src->read(m_base + qint64(e.offsetBlocks) * 512, 16);
                if (rh.size() == 16) {
                    const uint8_t* q = reinterpret_cast<const uint8_t*>(rh.constData());
                    e.sizeField = uint32_t(q[7]) | uint32_t(q[14]) << 8 | uint32_t(q[5]) << 16 | uint32_t(q[2]) << 24;
                }
            }
        }
    }
    if (m_entries[0].type != RpfEntry::Directory) return fail("root entry is not a directory (wrong decryption key?)");

    // link parents
    std::deque<int> q{0};
    std::vector<bool> seen(count, false);
    seen[0] = true;
    while (!q.empty()) {
        int d = q.front();
        q.pop_front();
        const RpfEntry& de = m_entries[size_t(d)];
        if (qint64(de.entriesIndex) + de.entriesCount > count) return fail("directory entry out of range");
        for (uint32_t c = de.entriesIndex; c < de.entriesIndex + de.entriesCount; ++c) {
            if (seen[c]) continue;
            seen[c] = true;
            m_entries[c].parent = d;
            if (m_entries[c].type == RpfEntry::Directory) q.push_back(int(c));
        }
    }
    return true;
}

std::vector<int> RpfArchive::children(int dir) const
{
    std::vector<int> out;
    const RpfEntry& d = entry(dir);
    if (!d.isDir()) return out;
    for (uint32_t c = d.entriesIndex; c < d.entriesIndex + d.entriesCount && c < m_entries.size(); ++c)
        if (m_entries[c].parent == dir) out.push_back(int(c));
    return out;
}

QString RpfArchive::entryPath(int i) const
{
    QStringList parts;
    while (i > 0) {
        parts.prepend(m_entries[size_t(i)].name);
        i = m_entries[size_t(i)].parent;
    }
    return parts.join('/');
}

int RpfArchive::findChild(int dir, const QString& name) const
{
    for (int c : children(dir))
        if (m_entries[size_t(c)].name.compare(name, Qt::CaseInsensitive) == 0) return c;
    return -1;
}

int RpfArchive::findPath(const QString& rel) const
{
    QString p = rel;
    p.replace('\\', '/');
    int cur = 0;
    for (const QString& part : p.split('/', Qt::SkipEmptyParts)) {
        cur = findChild(cur, part);
        if (cur < 0) return -1;
    }
    return cur;
}

bool RpfArchive::isArchiveEntry(int i) const
{
    const RpfEntry& e = entry(i);
    return e.type == RpfEntry::Binary && e.name.endsWith(QLatin1String(".rpf"), Qt::CaseInsensitive);
}

std::shared_ptr<RpfArchive> RpfArchive::child(int i, QString* err)
{
    std::lock_guard<std::mutex> lk(m_childMutex);
    auto it = m_childCache.find(i);
    if (it != m_childCache.end()) {
        if (auto c = it->second.lock()) return c;
    }
    auto c = openNested(shared_from_this(), i, err);
    if (c) m_childCache[i] = c;
    return c;
}

QByteArray RpfArchive::readStored(int i)
{
    const RpfEntry& e = entry(i);
    if (e.isDir()) return {};
    return m_src->read(m_base + qint64(e.offsetBlocks) * 512, e.storedSize());
}

bool RpfArchive::extract(int i, QByteArray& out, QString* err)
{
    const RpfEntry& e = entry(i);
    auto fail = [&](const QString& m) { if (err) *err = e.name + ": " + m; return false; };
    if (e.isDir()) return fail("is a directory");
    const GameKeys& keys = GameKeys::global();

    if (e.type == RpfEntry::Binary) {
        QByteArray d = readStored(i);
        if (d.size() != e.storedSize()) return fail("read error");
        if (e.encrypted) {
            if (m_encryption == RpfEncAes) {
                if (!keys.hasAes()) return fail("AES key required");
                aesDecrypt(d, keys.aesKey);
            } else {
                if (!keys.hasNg()) return fail("NG keys required");
                ngDecrypt(d, e.name, e.uncompressedSize, keys);
            }
        }
        if (e.isCompressed()) {
            if (!inflateRaw(d, out, e.uncompressedSize)) return fail("decompression failed");
        } else {
            out = d;
        }
        return true;
    }

    // resource
    if (e.sizeField < 16) return fail("resource too small");
    QByteArray body = m_src->read(m_base + qint64(e.offsetBlocks) * 512 + 16, qint64(e.sizeField) - 16);
    if (body.size() != int(e.sizeField - 16)) return fail("read error");
    if (e.encrypted) {
        if (m_encryption == RpfEncAes) {
            if (!keys.hasAes()) return fail("AES key required");
            aesDecrypt(body, keys.aesKey);
        } else {
            if (!keys.hasNg()) return fail("NG keys required");
            ngDecrypt(body, e.name, e.sizeField, keys);
        }
    }
    out = makeRsc7Header(e.resourceVersion(), e.systemFlags, e.graphicsFlags) + body;
    return true;
}

} // namespace rs
