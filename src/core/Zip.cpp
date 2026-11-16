#include "Zip.h"
#include "BinaryIO.h"
#include "Compression.h"
#include <QFile>

namespace rs {

bool ZipReader::open(const QString& path, QString* err)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) { if (err) *err = "Cannot open " + path; return false; }
    return openData(f.readAll(), err);
}

bool ZipReader::openData(const QByteArray& data, QString* err)
{
    m_data = data;
    m_entries.clear();
    const char* d = m_data.constData();
    const int n = m_data.size();
    int eocd = -1;
    for (int i = n - 22; i >= std::max(0, n - 22 - 65535); --i) {
        if (rd32(d + i) == 0x06054b50) { eocd = i; break; }
    }
    if (eocd < 0) { if (err) *err = "not a ZIP archive"; return false; }
    uint16_t count = rd16(d + eocd + 10);
    uint32_t cdOff = rd32(d + eocd + 16);
    qint64 p = cdOff;
    for (int i = 0; i < count; ++i) {
        if (p + 46 > n || rd32(d + p) != 0x02014b50) { if (err) *err = "corrupt ZIP central directory"; return false; }
        Entry e;
        uint16_t flags = rd16(d + p + 8);
        e.method = rd16(d + p + 10);
        e.csize = rd32(d + p + 20);
        e.usize = rd32(d + p + 24);
        uint16_t nl = rd16(d + p + 28), xl = rd16(d + p + 30), cl = rd16(d + p + 32);
        e.localOffset = rd32(d + p + 42);
        if (p + 46 + nl > n) { if (err) *err = "corrupt ZIP entry"; return false; }
        QByteArray rawName(d + p + 46, nl);
        e.name = (flags & 0x800) ? QString::fromUtf8(rawName) : QString::fromLatin1(rawName);
        e.name.replace('\\', '/');
        m_entries.push_back(e);
        p += 46 + nl + xl + cl;
    }
    return true;
}

QStringList ZipReader::names() const
{
    QStringList l;
    for (auto& e : m_entries) l << e.name;
    return l;
}

int ZipReader::find(const QString& nameIn) const
{
    QString name = nameIn;
    name.replace('\\', '/');
    while (name.startsWith('/')) name.remove(0, 1);
    for (size_t i = 0; i < m_entries.size(); ++i)
        if (m_entries[i].name.compare(name, Qt::CaseInsensitive) == 0) return int(i);
    return -1;
}

bool ZipReader::read(const QString& name, QByteArray& out, QString* err) const
{
    int i = find(name);
    if (i < 0) { if (err) *err = "missing in package: " + name; return false; }
    const Entry& e = m_entries[size_t(i)];
    const char* d = m_data.constData();
    qint64 p = e.localOffset;
    if (p + 30 > m_data.size() || rd32(d + p) != 0x04034b50) { if (err) *err = "corrupt ZIP local header"; return false; }
    qint64 start = p + 30 + rd16(d + p + 26) + rd16(d + p + 28);
    if (start + e.csize > m_data.size()) { if (err) *err = "truncated ZIP entry " + name; return false; }
    QByteArray c = m_data.mid(int(start), int(e.csize));
    if (e.method == 0) { out = c; return true; }
    if (e.method == 8) {
        if (!inflateRaw(c, out, e.usize)) { if (err) *err = "decompression failed: " + name; return false; }
        return true;
    }
    if (err) *err = QStringLiteral("unsupported ZIP compression method %1 for %2").arg(e.method).arg(name);
    return false;
}

} // namespace rs
