#include "GameFS.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>

namespace rs {

QString GameFS::modsDir() const { return QDir(gameDir).filePath("mods"); }

static QStringList splitPath(const QString& p)
{
    QString s = p;
    s.replace('\\', '/');
    return s.split('/', Qt::SkipEmptyParts);
}

// Case-insensitive path resolution (Linux file systems are case sensitive,
// while GTA paths in packages are not).
static QString resolveCase(const QString& base, const QStringList& parts, int count)
{
    QString cur = base;
    for (int i = 0; i < count; ++i) {
        QString cand = QDir(cur).filePath(parts[i]);
        if (QFileInfo::exists(cand)) { cur = cand; continue; }
        QString found;
        const auto names = QDir(cur).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden);
        for (const QString& n : names)
            if (n.compare(parts[i], Qt::CaseInsensitive) == 0) { found = n; break; }
        cur = QDir(cur).filePath(found.isEmpty() ? parts[i] : found);
    }
    return cur;
}

VirtualLocation GameFS::locate(const QString& rel) const
{
    VirtualLocation loc;
    QStringList parts = splitPath(rel);
    for (int i = 0; i < parts.size(); ++i) {
        if (parts[i].endsWith(QLatin1String(".rpf"), Qt::CaseInsensitive) && i < parts.size() - 1) {
            loc.archiveRel = QStringList(parts.mid(0, i + 1)).join('/');
            loc.innerPath = QStringList(parts.mid(i + 1)).join('/');
            loc.diskPath = resolveCase(gameDir, parts, i + 1);
            return loc;
        }
    }
    loc.diskPath = resolveCase(gameDir, parts, parts.size());
    return loc;
}

QString GameFS::archiveForEdit(const QString& archiveRel) const
{
    QStringList parts = splitPath(archiveRel);
    if (useModsFolder) return resolveCase(modsDir(), parts, parts.size());
    return resolveCase(gameDir, parts, parts.size());
}

bool GameFS::ensureModsCopy(const QString& archiveRel, QString& editPath, QString* err) const
{
    editPath = archiveForEdit(archiveRel);
    if (!useModsFolder || QFileInfo::exists(editPath)) return true;
    QStringList parts = splitPath(archiveRel);
    QString original = resolveCase(gameDir, parts, parts.size());
    if (!QFileInfo::exists(original)) return true;  // new archive: created on commit
    QDir().mkpath(QFileInfo(editPath).absolutePath());
    if (!QFile::copy(original, editPath)) {
        if (err) *err = "Failed copying " + original + " to " + editPath;
        return false;
    }
    QFile(editPath).setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ReadGroup | QFile::ReadOther);
    return true;
}

std::shared_ptr<RpfArchive> openArchiveChain(const std::shared_ptr<RpfArchive>& top, const QString& innerPath,
                                             QString& rest, QString* err)
{
    auto cur = top;
    QStringList parts = splitPath(innerPath);
    QStringList acc;
    for (int i = 0; i < parts.size(); ++i) {
        acc << parts[i];
        if (i < parts.size() - 1 && parts[i].endsWith(QLatin1String(".rpf"), Qt::CaseInsensitive)) {
            int idx = cur->findPath(acc.join('/'));
            if (idx < 0) { if (err) *err = "Not found: " + acc.join('/') + " in " + cur->name(); return nullptr; }
            cur = cur->child(idx, err);
            if (!cur) return nullptr;
            acc.clear();
        }
    }
    rest = acc.join('/');
    return cur;
}

bool GameFS::readFile(const QString& rel, QByteArray& out, QString* err) const
{
    VirtualLocation loc = locate(rel);
    if (!loc.inArchive()) {
        QFile f(loc.diskPath);
        if (!f.open(QIODevice::ReadOnly)) { if (err) *err = "Cannot read " + loc.diskPath; return false; }
        out = f.readAll();
        return true;
    }
    QString disk = loc.diskPath;
    if (useModsFolder) {
        QString m = archiveForEdit(loc.archiveRel);
        if (QFileInfo::exists(m)) disk = m;
    }
    auto top = RpfArchive::openFile(disk, err);
    if (!top) return false;
    QString rest;
    auto arc = openArchiveChain(top, loc.innerPath, rest, err);
    if (!arc) return false;
    int idx = arc->findPath(rest);
    if (idx < 0) { if (err) *err = "Not found: " + rel; return false; }
    return arc->extract(idx, out, err);
}

bool GameFS::isGameDir(const QString& dir)
{
    QDir d(dir);
    return d.exists("x64a.rpf") || d.exists("common.rpf") || d.exists("GTA5.exe") || d.exists("GTA5_Enhanced.exe");
}

QStringList GameFS::guessGameDirs()
{
    QStringList cands;
    QString home = QDir::homePath();
    const QStringList steamRoots = {home + "/.steam/steam", home + "/.local/share/Steam",
                                    home + "/.var/app/com.valvesoftware.Steam/.local/share/Steam"};
    for (const QString& root : steamRoots) {
        cands << root + "/steamapps/common/Grand Theft Auto V";
        cands << root + "/steamapps/common/Grand Theft Auto V Legacy";
        cands << root + "/steamapps/common/Grand Theft Auto V Enhanced";
        // extra library folders
        QFile lib(root + "/steamapps/libraryfolders.vdf");
        if (lib.open(QIODevice::ReadOnly)) {
            for (const QByteArray& line : lib.readAll().split('\n')) {
                int k = line.indexOf("\"path\"");
                if (k < 0) continue;
                QList<QByteArray> q = line.mid(k + 6).split('"');
                if (q.size() >= 2) {
                    QString p = QString::fromUtf8(q[1]);
                    cands << p + "/steamapps/common/Grand Theft Auto V" << p + "/steamapps/common/Grand Theft Auto V Legacy"
                          << p + "/steamapps/common/Grand Theft Auto V Enhanced";
                }
            }
        }
    }
    cands << home + "/Games/epic-games-store/drive_c/Program Files/Epic Games/GTAV"
          << home + "/Games/Heroic/GTAV" << home + "/Games/rockstar-games-launcher/drive_c/Program Files/Rockstar Games/Grand Theft Auto V";
    QStringList out;
    for (const QString& c : cands)
        if (isGameDir(c) && !out.contains(QFileInfo(c).canonicalFilePath())) out << QFileInfo(c).canonicalFilePath();
    return out;
}

} // namespace rs
