#include "Oiv.h"
#include "RpfWriter.h"
#include "XPathLite.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <map>

namespace rs {

static QString normPath(QString p)
{
    p.replace('\\', '/');
    while (p.contains("//")) p.replace("//", "/");
    while (p.startsWith('/')) p.remove(0, 1);
    while (p.endsWith('/')) p.chop(1);
    return p;
}

static QString joinPath(const QString& a, const QString& b)
{
    if (a.isEmpty()) return normPath(b);
    if (b.isEmpty()) return normPath(a);
    return normPath(a + "/" + b);
}

bool OivPackage::open(const QString& path, QString* err)
{
    if (!zip.open(path, err)) return false;
    QByteArray xml;
    if (!zip.read("assembly.xml", xml, err)) return false;
    QString perr;
    int line = 0;
    if (!assembly.setContent(xml, &perr, &line)) {
        if (err) *err = QStringLiteral("assembly.xml: %1 (line %2)").arg(perr).arg(line);
        return false;
    }
    QDomElement pkg = assembly.documentElement();
    if (pkg.tagName() != "package") { if (err) *err = "assembly.xml has no <package> root"; return false; }
    formatVersion = pkg.attribute("version");
    target = pkg.attribute("target");
    if (!target.isEmpty() && target.compare("Five", Qt::CaseInsensitive) != 0) {
        if (err) *err = "This package targets '" + target + "', not GTA V (Five)";
        return false;
    }
    if (formatVersion.startsWith("1")) {
        if (err) *err = "Package format version " + formatVersion + " is not supported (2.x required)";
        return false;
    }
    QDomElement md = pkg.firstChildElement("metadata");
    name = md.firstChildElement("name").text().trimmed();
    QDomElement ver = md.firstChildElement("version");
    versionText = ver.firstChildElement("major").text().trimmed() + "." + ver.firstChildElement("minor").text().trimmed();
    QString tag = ver.firstChildElement("tag").text().trimmed();
    if (!tag.isEmpty()) versionText += " " + tag;
    author = md.firstChildElement("author").firstChildElement("displayName").text().trimmed();
    description = md.firstChildElement("description").text().trimmed();
    licence = md.firstChildElement("licence").text().trimmed();
    QByteArray iconData;
    if (zip.contains("icon.png") && zip.read("icon.png", iconData, nullptr)) icon.loadFromData(iconData);
    if (content().isNull()) { if (err) *err = "assembly.xml has no <content>"; return false; }
    return true;
}

QDomElement OivPackage::content() const { return assembly.documentElement().firstChildElement("content"); }

static void describe(const QDomElement& parent, const QString& prefix, QStringList& out)
{
    for (QDomElement e = parent.firstChildElement(); !e.isNull(); e = e.nextSiblingElement()) {
        QString t = e.tagName();
        if (t == "add") out << "Add " + joinPath(prefix, e.text().trimmed());
        else if (t == "delete") out << "Delete " + joinPath(prefix, e.text().trimmed());
        else if (t == "archive") describe(e, joinPath(prefix, e.attribute("path")), out);
        else if (t == "text") out << "Edit text " + joinPath(prefix, e.attribute("path"));
        else if (t == "xml") out << "Edit XML " + joinPath(prefix, e.attribute("path"));
        else if (t == "defaultArchive") describe(e, prefix, out);
        else out << "(unsupported command <" + t + ">)";
    }
}

QStringList OivPackage::describeOperations() const
{
    QStringList out;
    describe(content(), QString(), out);
    return out;
}

// ------------------------------------------------------------------ text/xml commands

QString applyTextCommands(const QString& original, const QDomElement& te, QStringList* notes)
{
    QString nl = original.contains("\r\n") ? "\r\n" : "\n";
    QStringList lines = original.split(QRegularExpression("\r?\n"));
    bool trailing = !lines.isEmpty() && lines.last().isEmpty();
    if (trailing) lines.removeLast();
    auto findLine = [&](const QString& l) {
        QString want = l.trimmed();
        for (int i = 0; i < lines.size(); ++i)
            if (lines[i].trimmed() == want) return i;
        return -1;
    };
    for (QDomElement c = te.firstChildElement(); !c.isNull(); c = c.nextSiblingElement()) {
        QString t = c.tagName();
        QString text = c.text();
        if (t == "add") {
            lines << text;
        } else if (t == "insert") {
            int i = findLine(c.attribute("line"));
            if (i < 0) { if (notes) *notes << "insert: line not found: " + c.attribute("line"); lines << text; continue; }
            bool before = c.attribute("where").compare("Before", Qt::CaseInsensitive) == 0;
            lines.insert(before ? i : i + 1, text);
        } else if (t == "replace") {
            int i = findLine(c.attribute("line"));
            if (i < 0) { if (notes) *notes << "replace: line not found: " + c.attribute("line"); continue; }
            lines[i] = text;
        } else if (t == "delete") {
            int i = findLine(text);
            if (i < 0) { if (notes) *notes << "delete: line not found: " + text; continue; }
            lines.removeAt(i);
        } else if (notes) {
            *notes << "unsupported text command <" + t + ">";
        }
    }
    return lines.join(nl) + (trailing || original.isEmpty() ? nl : QString());
}

static QList<QDomNode> fragmentNodes(QDomDocument& doc, const QDomElement& cmd, QString* err)
{
    QList<QDomNode> out;
    bool hasNodes = false;
    for (QDomNode n = cmd.firstChild(); !n.isNull(); n = n.nextSibling())
        if (n.isElement() || n.isComment()) hasNodes = true;
    if (hasNodes) {
        for (QDomNode n = cmd.firstChild(); !n.isNull(); n = n.nextSibling())
            if (n.isElement() || n.isComment() || (n.isText() && !n.nodeValue().trimmed().isEmpty())) out << doc.importNode(n, true);
        return out;
    }
    QString text = cmd.text().trimmed();
    if (text.isEmpty()) return out;
    QDomDocument frag;
    if (frag.setContent("<__frag>" + text + "</__frag>")) {
        for (QDomNode n = frag.documentElement().firstChild(); !n.isNull(); n = n.nextSibling()) out << doc.importNode(n, true);
    } else {
        out << doc.createTextNode(text);
    }
    (void)err;
    return out;
}

bool applyXmlCommands(QByteArray& xml, const QDomElement& xe, QString* err, QStringList* notes)
{
    QDomDocument doc;
    QString perr;
    int line = 0;
    if (!doc.setContent(xml, &perr, &line)) {
        if (err) *err = QStringLiteral("XML parse error: %1 (line %2)").arg(perr).arg(line);
        return false;
    }
    for (QDomElement c = xe.firstChildElement(); !c.isNull(); c = c.nextSiblingElement()) {
        QString t = c.tagName();
        QString xp = c.attribute("xpath");
        QString xerr;
        QList<QDomNode> targets = xpathSelect(doc, xp, &xerr);
        if (!xerr.isEmpty()) { if (err) *err = xerr; return false; }
        if (targets.isEmpty()) { if (notes) *notes << t + ": nothing matches " + xp; continue; }
        if (t == "add") {
            bool first = c.attribute("append").compare("First", Qt::CaseInsensitive) == 0;
            for (QDomNode target : targets) {
                QList<QDomNode> nodes = fragmentNodes(doc, c, err);
                QDomNode anchor = target.firstChild();
                for (QDomNode n : nodes) {
                    if (first && !anchor.isNull()) target.insertBefore(n, anchor);
                    else target.appendChild(n);
                }
            }
        } else if (t == "replace") {
            for (QDomNode target : targets) {
                if (target.isAttr()) { target.setNodeValue(c.text()); continue; }
                QDomNode parent = target.parentNode();
                QList<QDomNode> nodes = fragmentNodes(doc, c, err);
                for (QDomNode n : nodes) parent.insertBefore(n, target);
                parent.removeChild(target);
            }
        } else if (t == "remove") {
            for (QDomNode target : targets) {
                if (target.isAttr()) target.toAttr().ownerElement().removeAttributeNode(target.toAttr());
                else target.parentNode().removeChild(target);
            }
        } else if (notes) {
            *notes << "unsupported xml command <" + t + ">";
        }
    }
    // Keep the original XML declaration verbatim (QDom rewrites its quoting).
    QByteArray decl;
    QByteArray trimmed = xml.trimmed();
    if (trimmed.startsWith("\xEF\xBB\xBF")) trimmed = trimmed.mid(3);
    if (trimmed.startsWith("<?xml")) decl = trimmed.left(trimmed.indexOf("?>") + 2) + "\n";
    QByteArray body = doc.toByteArray(2);
    if (body.startsWith("<?xml")) body = body.mid(body.indexOf("?>") + 2).trimmed() + "\n";
    xml = decl + body;
    return true;
}

// ------------------------------------------------------------------ installer

namespace {

struct Ctx {
    QString archiveRel;   // "" for loose game files
    QString inner;        // path inside the archive chain
};

class Installer {
public:
    Installer(const GameFS& fs, const OivPackage& pkg, bool dry, std::function<void(const QString&)> log)
        : m_fs(fs), m_pkg(pkg), m_dry(dry), m_log(std::move(log)) {}

    bool run(QString* err)
    {
        if (!processChildren(m_pkg.content(), Ctx(), err)) return false;
        for (auto& kv : m_sessions) {
            say("Saving " + kv.second->diskPath());
            if (m_dry) continue;
            if (!kv.second->commit(err, [this](const QString& s) { say("  " + s); })) return false;
        }
        for (auto& n : m_notes) say("Note: " + n);
        return true;
    }

private:
    void say(const QString& s) { if (m_log) m_log(s); }

    ArchiveEditSession* session(const QString& archiveRel, bool create, QString* err)
    {
        QString key = archiveRel.toLower();
        auto it = m_sessions.find(key);
        if (it != m_sessions.end()) return it->second.get();
        QString path;
        if (m_dry) path = m_fs.archiveForEdit(archiveRel);
        else if (!m_fs.ensureModsCopy(archiveRel, path, err)) return nullptr;
        if (m_dry && !QFileInfo::exists(path)) path = m_fs.locate(archiveRel + "/x").diskPath;
        auto s = std::make_unique<ArchiveEditSession>();
        if (!s->begin(path, create, err)) return nullptr;
        say("Editing archive " + path);
        auto* raw = s.get();
        m_sessions[key] = std::move(s);
        return raw;
    }

    // Splits a game-relative path into archive + inner part.
    Ctx resolve(const Ctx& ctx, const QString& rel)
    {
        if (!ctx.archiveRel.isEmpty()) return {ctx.archiveRel, joinPath(ctx.inner, rel)};
        QString full = normPath(rel);
        QStringList parts = full.split('/', Qt::SkipEmptyParts);
        for (int i = 0; i < parts.size(); ++i)
            if (parts[i].endsWith(QLatin1String(".rpf"), Qt::CaseInsensitive))
                return {QStringList(parts.mid(0, i + 1)).join('/'), QStringList(parts.mid(i + 1)).join('/')};
        return {QString(), full};
    }

    bool readSource(const QDomElement& e, QByteArray& data, QString* err)
    {
        QString src = e.attribute("source");
        if (src.isEmpty()) { if (err) *err = "<add> without source attribute"; return false; }
        if (m_pkg.zip.contains("content/" + normPath(src))) return m_pkg.zip.read("content/" + normPath(src), data, err);
        return m_pkg.zip.read(normPath(src), data, err);
    }

    // Returns the archive tree and in-archive path for a file target.
    RpfNode* treeFor(const Ctx& c, QString& rest, bool create, QString* err)
    {
        ArchiveEditSession* s = session(c.archiveRel, true, err);
        if (!s) return nullptr;
        return s->resolve(c.inner, rest, create, err);
    }

    bool putFile(const Ctx& c, const QByteArray& data, QString* err)
    {
        if (c.archiveRel.isEmpty()) {
            QString dst = m_fs.locate(c.inner).diskPath;
            say("Write " + dst);
            if (m_dry) return true;
            QDir().mkpath(QFileInfo(dst).absolutePath());
            if (QFileInfo::exists(dst)) backupLoose(dst);
            QFile f(dst);
            if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate) || f.write(data) != data.size()) {
                if (err) *err = "Cannot write " + dst;
                return false;
            }
            return true;
        }
        QString rest;
        RpfNode* tree = treeFor(c, rest, true, err);
        if (!tree) return false;
        QString dir = QFileInfo(rest).path();
        RpfNode* d = (dir == "." || dir.isEmpty()) ? tree : tree->ensureDir(dir);
        if (!d) { if (err) *err = "Cannot create folder " + dir; return false; }
        say("Add " + c.archiveRel + "/" + c.inner);
        d->addChild(newFileNode(QFileInfo(rest).fileName(), data));
        return true;
    }

    bool getFile(const Ctx& c, QByteArray& data, bool& exists, QString* err)
    {
        exists = false;
        if (c.archiveRel.isEmpty()) {
            QFile f(m_fs.locate(c.inner).diskPath);
            if (!f.exists()) return true;
            if (!f.open(QIODevice::ReadOnly)) { if (err) *err = "Cannot read " + f.fileName(); return false; }
            data = f.readAll();
            exists = true;
            return true;
        }
        QString rest;
        RpfNode* tree = treeFor(c, rest, false, err);
        if (!tree) return false;
        RpfNode* n = tree->findPath(rest);
        if (!n || n->isDir()) return true;
        exists = true;
        return readNodeData(n, data, err);
    }

    void backupLoose(const QString& path)
    {
        QString rel = QDir(m_fs.gameDir).relativeFilePath(path);
        QString bak = QDir(m_fs.gameDir).filePath("rpfstudio_backup/" + rel);
        if (QFileInfo::exists(bak)) return;
        QDir().mkpath(QFileInfo(bak).absolutePath());
        QFile::copy(path, bak);
    }

    bool processChildren(const QDomElement& parent, const Ctx& ctx, QString* err)
    {
        for (QDomElement e = parent.firstChildElement(); !e.isNull(); e = e.nextSiblingElement()) {
            const QString t = e.tagName();
            if (t == "add") {
                QByteArray data;
                if (!readSource(e, data, err)) return false;
                if (!putFile(resolve(ctx, e.text().trimmed()), data, err)) return false;
            } else if (t == "delete") {
                Ctx c = resolve(ctx, e.text().trimmed());
                if (c.archiveRel.isEmpty()) {
                    QString p = m_fs.locate(c.inner).diskPath;
                    say("Delete " + p);
                    if (!m_dry && QFileInfo::exists(p)) { backupLoose(p); QFile::remove(p); }
                } else {
                    QString rest;
                    RpfNode* tree = treeFor(c, rest, false, err);
                    if (!tree) return false;
                    RpfNode* n = tree->findPath(rest);
                    if (n && n->parent) { say("Delete " + c.archiveRel + "/" + c.inner); n->parent->take(n); }
                    else m_notes << "delete: not found " + c.archiveRel + "/" + c.inner;
                }
            } else if (t == "archive") {
                QString type = e.attribute("type", "RPF7");
                if (type.compare("RPF7", Qt::CaseInsensitive) != 0) {
                    if (err) *err = "Unsupported archive type " + type;
                    return false;
                }
                bool create = e.attribute("createIfNotExist").compare("True", Qt::CaseInsensitive) == 0;
                Ctx c = resolve(ctx, e.attribute("path"));
                if (c.archiveRel.isEmpty()) { if (err) *err = "archive path is not an .rpf: " + e.attribute("path"); return false; }
                ArchiveEditSession* s = session(c.archiveRel, create, err);
                if (!s) return false;
                if (!c.inner.isEmpty()) {
                    QString rest;
                    // Opening the nested archive validates it exists (or creates it).
                    if (!s->resolve(c.inner + "/__probe__", rest, create, err)) return false;
                }
                if (!processChildren(e, c, err)) return false;
            } else if (t == "text" || t == "xml") {
                Ctx c = resolve(ctx, e.attribute("path"));
                bool create = e.attribute("createIfNotExist").compare("True", Qt::CaseInsensitive) == 0;
                QByteArray data;
                bool exists = false;
                if (!getFile(c, data, exists, err)) return false;
                if (!exists && !create) {
                    if (err) *err = "File to edit does not exist: " + e.attribute("path");
                    return false;
                }
                say("Edit " + (c.archiveRel.isEmpty() ? c.inner : c.archiveRel + "/" + c.inner));
                if (t == "text") {
                    data = applyTextCommands(QString::fromUtf8(data), e, &m_notes).toUtf8();
                } else {
                    if (!exists) data = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
                    if (!applyXmlCommands(data, e, err, &m_notes)) return false;
                }
                if (!putFile(c, data, err)) return false;
            } else if (t == "defaultArchive") {
                if (!processChildren(e, ctx, err)) return false;
            } else {
                m_notes << "ignored unsupported command <" + t + ">";
            }
        }
        return true;
    }

    const GameFS& m_fs;
    const OivPackage& m_pkg;
    bool m_dry;
    std::function<void(const QString&)> m_log;
    std::map<QString, std::unique_ptr<ArchiveEditSession>> m_sessions;
    QStringList m_notes;
};

} // namespace

bool OivInstaller::install(const OivPackage& pkg, QString* err)
{
    if (!QFileInfo(m_fs.gameDir).isDir()) {
        if (err) *err = "Game folder is not set";
        return false;
    }
    Installer inst(m_fs, pkg, dryRun, log);
    if (log) log(QStringLiteral("Installing \"%1\" %2 into %3%4").arg(pkg.name, pkg.versionText, m_fs.gameDir,
                                                                         m_fs.useModsFolder ? " (mods folder)" : ""));
    bool ok = inst.run(err);
    if (log) log(ok ? (dryRun ? "Dry run finished: nothing was written." : "Installation complete.") : "Installation failed: " + (err ? *err : QString()));
    return ok;
}

} // namespace rs
