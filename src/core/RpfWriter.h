// RPF7 archive writing and modification.
//
// Archives are always written with an unencrypted ("OPEN") table of contents,
// which is what OpenIV does too. Unchanged entries are copied byte-for-byte,
// so files that were NG encrypted stay encrypted.
#pragma once
#include "Rpf.h"
#include <QIODevice>
#include <functional>

namespace rs {

struct RpfNode {
    enum Source { Directory, FromEntry, FromBytes, FromDiskFile };
    QString name;
    Source source = Directory;
    std::vector<std::unique_ptr<RpfNode>> children;
    RpfNode* parent = nullptr;

    // FromEntry: an entry of an existing archive (copied raw)
    std::shared_ptr<RpfArchive> srcArchive;
    int srcEntry = -1;
    // FromBytes: game-file bytes (plain binary, or an RSC7 resource file)
    QByteArray bytes;
    // FromDiskFile: game-file bytes read from disk at write time
    QString diskPath;
    bool compress = true;  // binaries only; ignored for .rpf/.awc (stored)

    bool isDir() const { return source == Directory; }
    RpfNode* child(const QString& name) const;             // case-insensitive
    RpfNode* findPath(const QString& relPath);             // "a/b/c"
    RpfNode* ensureDir(const QString& relPath);            // creates missing dirs
    RpfNode* addChild(std::unique_ptr<RpfNode> n);         // replaces same-named child
    std::unique_ptr<RpfNode> take(RpfNode* c);
    QString path() const;
};

// Build an editable tree mirroring an existing archive.
std::unique_ptr<RpfNode> treeFromArchive(const std::shared_ptr<RpfArchive>& a);
std::unique_ptr<RpfNode> newDirNode(const QString& name = QString());
std::unique_ptr<RpfNode> newFileNode(const QString& name, const QByteArray& bytes);
std::unique_ptr<RpfNode> newDiskFileNode(const QString& name, const QString& diskPath);

using WriteProgress = std::function<void(const QString&)>;

// Writes a complete archive (full rebuild).
bool writeRpf(RpfNode& root, QIODevice& out, QString* err, const WriteProgress& progress = {});
bool writeRpfFile(RpfNode& root, const QString& path, QString* err, const WriteProgress& progress = {});

// Applies an edit to an archive (top-level or nested) and saves it to disk.
// The mutator edits the tree of `archive`. Top-level archives are updated in
// place (new data appended, table of contents rewritten) unless `fullRebuild`
// is set, which compacts the archive. Nested archives are rebuilt and written
// back into their parent. All RpfArchive objects for the affected file are
// stale afterwards and must be reopened.
using RpfMutator = std::function<bool(RpfNode& root, QString* err)>;
bool modifyArchive(const std::shared_ptr<RpfArchive>& archive, const RpfMutator& mutator, QString* err,
                   bool fullRebuild = false, const WriteProgress& progress = {});

// Creates a new, empty archive on disk.
bool createEmptyArchive(const QString& path, QString* err);

// Reads the game-file bytes a node currently represents.
bool readNodeData(const RpfNode* node, QByteArray& out, QString* err);

// Batched editing of one top-level archive and any archives nested in it.
// All changes are kept in memory and written by commit().
class ArchiveEditSession {
public:
    ArchiveEditSession();
    ~ArchiveEditSession();
    // Opens (or, with createIfMissing, creates) a top-level archive on disk.
    bool begin(const QString& diskPath, bool createIfMissing, QString* err);
    RpfNode* root();
    QString diskPath() const { return m_path; }
    // Resolves "a/b.rpf/c" style paths: returns the tree of the innermost
    // archive and the remaining path inside it. Nested archives are opened
    // (or created when `create` is set) on demand.
    RpfNode* resolve(const QString& innerPath, QString& rest, bool create, QString* err);
    bool commit(QString* err, const WriteProgress& progress = {});
private:
    struct Sub;
    RpfNode* openNested(RpfNode* tree, const QString& path, bool create, QString* err);
    QString m_path;
    std::shared_ptr<RpfArchive> m_archive;   // null for a new archive
    std::unique_ptr<RpfNode> m_root;
    std::vector<std::unique_ptr<Sub>> m_subs;
    std::vector<QString> m_tempFiles;
};

} // namespace rs
