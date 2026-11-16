// RPF7 archive reader (GTA V).
#pragma once
#include <QByteArray>
#include <QFile>
#include <QString>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace rs {

class GameKeys;

enum RpfEncryption : uint32_t {
    RpfEncNone = 0,
    RpfEncOpen = 0x4E45504F,  // "OPEN": unencrypted table of contents (OpenIV style)
    RpfEncAes = 0x0FFFFFF9,
    RpfEncNg = 0x0FEFFFFF,
};
static constexpr uint32_t RpfMagic = 0x52504637;   // "7FPR"
static constexpr uint32_t Rsc7Magic = 0x37435352;  // "RSC7"

QString encryptionName(uint32_t enc);

// Thread-safe positional reads from a file shared by an archive and its children.
class FileSource {
public:
    static std::shared_ptr<FileSource> open(const QString& path, QString* err);
    QByteArray read(qint64 offset, qint64 length);
    qint64 size() const { return m_size; }
    QString path() const { return m_path; }
private:
    QFile m_file;
    QString m_path;
    qint64 m_size = 0;
    std::mutex m_mutex;
};

struct RpfEntry {
    enum Type { Directory, Binary, Resource } type = Binary;
    QString name;
    int parent = -1;
    // directory
    uint32_t entriesIndex = 0, entriesCount = 0;
    // files
    uint32_t offsetBlocks = 0;      // in 512-byte blocks
    uint32_t sizeField = 0;         // binary: compressed size (0 = stored); resource: total stored size
    uint32_t uncompressedSize = 0;  // binary only
    bool encrypted = false;
    uint32_t systemFlags = 0, graphicsFlags = 0; // resource only

    bool isDir() const { return type == Directory; }
    bool isResource() const { return type == Resource; }
    bool isCompressed() const { return type == Binary && sizeField != 0; }
    // Bytes occupied in the archive.
    qint64 storedSize() const {
        if (type == Binary) return sizeField ? sizeField : uncompressedSize;
        return sizeField;
    }
    // Size of the extracted game file.
    qint64 fileSize() const { return type == Binary ? uncompressedSize : sizeField; }
    uint32_t resourceVersion() const { return ((systemFlags >> 28) & 0xF) << 4 | ((graphicsFlags >> 28) & 0xF); }
};

class RpfArchive : public std::enable_shared_from_this<RpfArchive> {
public:
    static std::shared_ptr<RpfArchive> openFile(const QString& path, QString* err);
    // Open an archive stored inside another one.
    static std::shared_ptr<RpfArchive> openNested(const std::shared_ptr<RpfArchive>& parent, int entry, QString* err);

    QString name() const { return m_name; }
    QString physicalPath() const { return m_src ? m_src->path() : QString(); }
    // Path of this archive inside the top-level archive ("" for top-level), '/' separated.
    QString internalPath() const { return m_internalPath; }
    QString displayPath() const;
    std::shared_ptr<RpfArchive> parent() const { return m_parent; }
    int parentEntry() const { return m_parentEntry; }
    std::shared_ptr<RpfArchive> topLevel();

    uint32_t encryption() const { return m_encryption; }
    qint64 baseOffset() const { return m_base; }
    qint64 size() const { return m_size; }
    const std::vector<RpfEntry>& entries() const { return m_entries; }
    const RpfEntry& entry(int i) const { return m_entries.at(size_t(i)); }
    int count() const { return int(m_entries.size()); }
    std::vector<int> children(int dir) const;

    QString entryPath(int i) const;          // "dir/sub/file.ext" relative to archive root
    int findPath(const QString& rel) const;  // case-insensitive, '/' or '\\'; -1 if missing
    int findChild(int dir, const QString& name) const;

    bool isArchiveEntry(int i) const;        // nested .rpf
    std::shared_ptr<RpfArchive> child(int i, QString* err);  // cached

    // Raw bytes as stored in the archive.
    QByteArray readStored(int i);
    // Game-file bytes: binaries decrypted and decompressed; resources returned as
    // a standalone RSC7 file (16-byte header + compressed body, decrypted).
    bool extract(int i, QByteArray& out, QString* err);

    std::shared_ptr<FileSource> source() const { return m_src; }

private:
    bool parse(QString* err);

    std::shared_ptr<FileSource> m_src;
    std::shared_ptr<RpfArchive> m_parent;
    int m_parentEntry = -1;
    QString m_name, m_internalPath;
    qint64 m_base = 0, m_size = 0;
    uint32_t m_encryption = 0;
    std::vector<RpfEntry> m_entries;
    std::map<int, std::weak_ptr<RpfArchive>> m_childCache;
    std::mutex m_childMutex;
};

// Builds the 16-byte RSC7 header for a resource entry.
QByteArray makeRsc7Header(uint32_t version, uint32_t sysFlags, uint32_t gfxFlags);

} // namespace rs
