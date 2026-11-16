// Game folder access: resolves virtual paths that pass through archives,
// e.g. "update/update.rpf/common/data/dlclist.xml", and handles the "mods"
// folder convention (edited archives live in <game>/mods/<same path>).
#pragma once
#include "Rpf.h"
#include <QString>
#include <memory>

namespace rs {

struct VirtualLocation {
    QString diskPath;      // loose file, or the top-level archive on disk
    QString archiveRel;    // top-level archive path relative to the base folder ("" if loose)
    QString innerPath;     // path inside the archive chain (may pass through nested .rpf), "" if loose
    bool inArchive() const { return !innerPath.isEmpty() || !archiveRel.isEmpty(); }
};

class GameFS {
public:
    QString gameDir;
    bool useModsFolder = true;

    QString modsDir() const;
    // Splits a game-relative path at the first .rpf component.
    VirtualLocation locate(const QString& gameRelPath) const;
    // Where an archive should be edited: the mods copy if enabled.
    QString archiveForEdit(const QString& archiveRel) const;
    // Copies the original archive into mods/ if missing. Returns the path to edit.
    bool ensureModsCopy(const QString& archiveRel, QString& editPath, QString* err) const;

    // Reads a file through archives (prefers the mods copy when it exists).
    bool readFile(const QString& gameRelPath, QByteArray& out, QString* err) const;

    static bool isGameDir(const QString& dir);
    static QStringList guessGameDirs();
};

// Opens the archive chain "a.rpf/b.rpf" starting from a top-level archive.
std::shared_ptr<RpfArchive> openArchiveChain(const std::shared_ptr<RpfArchive>& top, const QString& innerPath,
                                             QString& rest, QString* err);

} // namespace rs
