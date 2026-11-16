// Minimal read-only ZIP support (stored and deflated entries), used for .oiv packages.
#pragma once
#include <QByteArray>
#include <QString>
#include <QStringList>
#include <vector>

namespace rs {

class ZipReader {
public:
    bool open(const QString& path, QString* err);
    bool openData(const QByteArray& data, QString* err);
    QStringList names() const;
    bool contains(const QString& name) const { return find(name) >= 0; }
    // Case-insensitive lookup, '/' or '\\' separators.
    bool read(const QString& name, QByteArray& out, QString* err) const;
private:
    struct Entry { QString name; uint16_t method; uint32_t csize, usize, localOffset; };
    int find(const QString& name) const;
    QByteArray m_data;
    std::vector<Entry> m_entries;
};

} // namespace rs
