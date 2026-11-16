// rpfstudio-cli: scriptable access to the RpfStudio core.
#include "Crypto.h"
#include "Drawable.h"
#include "GameFS.h"
#include "Gxt2.h"
#include "Oiv.h"
#include "Rpf.h"
#include "RpfWriter.h"
#include "Texture.h"
#include "Resource.h"
#include "Ytd.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTextStream>
#include <cstdio>
#include <functional>
#include <map>

using namespace rs;

static QTextStream out(stdout);
static QTextStream errs(stderr);

static int fail(const QString& m)
{
    errs << "error: " << m << Qt::endl;
    return 1;
}

static QString defaultKeyDir()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)).filePath("RpfStudio/keys");
}

static bool readFile(const QString& p, QByteArray& d)
{
    QFile f(p);
    if (!f.open(QIODevice::ReadOnly)) return false;
    d = f.readAll();
    return true;
}

static bool writeFile(const QString& p, const QByteArray& d)
{
    QDir().mkpath(QFileInfo(p).absolutePath());
    QFile f(p);
    return f.open(QIODevice::WriteOnly | QIODevice::Truncate) && f.write(d) == d.size();
}

static void usage()
{
    out << "RpfStudio command line\n\n"
           "Usage: rpfstudio-cli [--keys DIR] <command> [args]\n\n"
           "Keys (needed for encrypted game archives):\n"
           "  keys-find <GTA5.exe> [magic.dat]      locate keys in your game executable, save to key dir\n"
           "  keys-info                              show loaded keys\n\n"
           "Archives (INNER paths may pass through nested archives, e.g. x64/vehicles.rpf/adder.yft):\n"
           "  info <archive>\n"
           "  list <archive> [INNER] [-r]\n"
           "  extract <archive> <INNER|.> <outdir>  extract a file or folder (\".\" = everything)\n"
           "  cat <archive> <INNER>                 write a file to stdout\n"
           "  add <archive> <INNER-dir> <files/dirs...>  add or replace files\n"
           "  delete <archive> <INNER>\n"
           "  mkdir <archive> <INNER>\n"
           "  rename <archive> <INNER> <new-name>\n"
           "  new <archive>                          create an empty archive\n"
           "  pack <folder> <archive>                folders named *.rpf become nested archives\n"
           "  defrag <archive> [INNER-archive]       rebuild and compact\n"
           "  verify <archive> [-v]                  extract every file (incl. nested archives) in memory, report errors\n\n"
           "Formats:\n"
           "  ytd-list <file.ytd>\n"
           "  ytd-export <file.ytd> <outdir> [--png]\n"
           "  ytd-build <out.ytd> <images...>        DDS keep their format; PNG/JPG etc. become DXT5\n"
           "  ytd-replace <file.ytd> <image> [name]  add or replace one texture in place\n"
           "  model-obj <file.ydr|ydd|yft> <out.obj>\n"
           "  gxt2-dump <file.gxt2>\n"
           "  gxt2-build <in.txt> <out.gxt2>\n"
           "  rbf2xml <file>\n\n"
           "Mod packages:\n"
           "  oiv-info <package.oiv>\n"
           "  oiv-install <package.oiv> --game <dir> [--no-mods] [--dry-run]\n";
}

static std::shared_ptr<RpfArchive> openChain(const QString& archive, const QString& inner, QString& rest, QString* err)
{
    auto top = RpfArchive::openFile(archive, err);
    if (!top) return nullptr;
    return openArchiveChain(top, inner, rest, err);
}

// Opens the archive that `inner` designates when it names an .rpf (or "" = top).
static std::shared_ptr<RpfArchive> openTarget(const QString& archive, const QString& inner, QString* err)
{
    QString rest;
    auto a = openChain(archive, inner, rest, err);
    if (!a) return nullptr;
    if (rest.isEmpty()) return a;
    int idx = a->findPath(rest);
    if (idx >= 0 && a->isArchiveEntry(idx)) return a->child(idx, err);
    if (err) *err = "not an archive: " + inner;
    return nullptr;
}

static void listDir(const std::shared_ptr<RpfArchive>& a, int dir, bool recursive, const QString& prefix)
{
    for (int c : a->children(dir)) {
        const RpfEntry& e = a->entry(c);
        QString path = prefix.isEmpty() ? e.name : prefix + "/" + e.name;
        if (e.isDir()) {
            out << QStringLiteral("%1  %2/\n").arg("<dir>", 12).arg(path);
            if (recursive) listDir(a, c, true, path);
        } else {
            QString kind = e.isResource() ? QStringLiteral("R v%1").arg(e.resourceVersion()) : (e.isCompressed() ? "C" : "-");
            if (e.encrypted) kind += " E";
            out << QStringLiteral("%1  %2  [%3]\n").arg(e.fileSize(), 12).arg(path, kind);
        }
    }
}

static int extractEntry(const std::shared_ptr<RpfArchive>& a, int idx, const QString& outDir, int& count)
{
    const RpfEntry& e = a->entry(idx);
    if (e.isDir()) {
        QString d = idx == 0 ? outDir : QDir(outDir).filePath(e.name);
        QDir().mkpath(d);
        for (int c : a->children(idx))
            if (int r = extractEntry(a, c, d, count)) return r;
        return 0;
    }
    QByteArray data;
    QString err;
    if (!a->extract(idx, data, &err)) { errs << "warning: " << err << Qt::endl; return 0; }
    if (!writeFile(QDir(outDir).filePath(e.name), data)) return fail("cannot write " + QDir(outDir).filePath(e.name));
    ++count;
    return 0;
}

static void addPathToNode(RpfNode& dir, const QString& path)
{
    QFileInfo fi(path);
    if (fi.isDir()) {
        RpfNode* d = dir.child(fi.fileName());
        if (!d || !d->isDir()) d = dir.addChild(newDirNode(fi.fileName()));
        for (const QFileInfo& c : QDir(path).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot, QDir::Name))
            addPathToNode(*d, c.absoluteFilePath());
    } else {
        dir.addChild(newDiskFileNode(fi.fileName(), fi.absoluteFilePath()));
    }
}

// pack: directories named *.rpf are written as nested archives
static bool packFolder(const QString& folder, RpfNode& node, QStringList& temps, QString* err)
{
    for (const QFileInfo& c : QDir(folder).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot, QDir::Name)) {
        if (c.isDir() && c.fileName().endsWith(".rpf", Qt::CaseInsensitive)) {
            auto sub = newDirNode();
            if (!packFolder(c.absoluteFilePath(), *sub, temps, err)) return false;
            QString tmp = QDir::temp().filePath(QStringLiteral("rpfstudio-pack-%1-%2").arg(temps.size()).arg(QCoreApplication::applicationPid()));
            if (!writeRpfFile(*sub, tmp, err)) return false;
            temps << tmp;
            auto n = newDiskFileNode(c.fileName(), tmp);
            n->compress = false;
            node.addChild(std::move(n));
        } else if (c.isDir()) {
            RpfNode* d = node.addChild(newDirNode(c.fileName()));
            if (!packFolder(c.absoluteFilePath(), *d, temps, err)) return false;
        } else {
            node.addChild(newDiskFileNode(c.fileName(), c.absoluteFilePath()));
        }
    }
    return true;
}

static bool loadImageAsTexture(const QString& path, TextureImage& t, QString* err)
{
    QByteArray d;
    if (!readFile(path, d)) { if (err) *err = "cannot read " + path; return false; }
    if (path.endsWith(".dds", Qt::CaseInsensitive)) return readDds(d, t, err);
    QImage img;
    if (!img.loadFromData(d)) { if (err) *err = "unsupported image: " + path; return false; }
    t = encodeImage(img, img.hasAlphaChannel() ? TexDXT5 : TexDXT1, true);
    return true;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QStringList args = app.arguments().mid(1);
    QString keyDir = defaultKeyDir();
    int ki = args.indexOf("--keys");
    if (ki >= 0 && ki + 1 < args.size()) { keyDir = args[ki + 1]; args.remove(ki, 2); }
    if (args.isEmpty() || args[0] == "-h" || args[0] == "--help" || args[0] == "help") { usage(); return args.isEmpty() ? 1 : 0; }
    GameKeys& keys = GameKeys::global();
    if (QDir(keyDir).exists()) keys.loadFromDir(keyDir, nullptr);

    const QString cmd = args[0];
    auto need = [&](int n) { if (args.size() < n + 1) { usage(); return false; } return true; };
    QString err;

    if (cmd == "keys-info") {
        out << "Key folder: " << keyDir << "\n" << keys.summary() << Qt::endl;
        return 0;
    }
    if (cmd == "keys-find") {
        if (!need(1)) return 1;
        if (!keys.searchExecutable(args[1], [](const QString& s) { errs << s << "\r"; errs.flush(); }, &err)) return fail(err);
        errs << Qt::endl;
        if (args.size() > 2 && !keys.loadMagicDat(args[2], &err)) return fail(err);
        if (!keys.hasAes() && !keys.hasNg()) return fail("no keys found in " + args[1]);
        if (!keys.saveToDir(keyDir, &err)) return fail(err);
        out << keys.summary() << "\nSaved to " << keyDir << Qt::endl;
        if (!keys.hasNg()) out << "NG keys were not found in this executable. Supply CodeWalker's magic.dat as a second argument,\n"
                                  "or copy gtav_ng_key.dat / gtav_ng_decrypt_tables.dat / gtav_hash_lut.dat into the key folder." << Qt::endl;
        return 0;
    }
    if (cmd == "info") {
        if (!need(1)) return 1;
        auto a = openTarget(args[1], args.value(2), &err);
        if (!a) return fail(err);
        int files = 0, dirs = 0, res = 0;
        for (auto& e : a->entries()) { if (e.isDir()) ++dirs; else { ++files; if (e.isResource()) ++res; } }
        out << "Archive:    " << a->displayPath() << "\nSize:       " << a->size() << "\nEncryption: " << encryptionName(a->encryption())
            << "\nEntries:    " << a->count() << " (" << dirs << " folders, " << files << " files, " << res << " resources)" << Qt::endl;
        return 0;
    }
    if (cmd == "list") {
        if (!need(1)) return 1;
        bool rec = args.removeAll("-r") > 0;
        QString inner = args.value(2);
        QString rest;
        auto a = openChain(args[1], inner, rest, &err);
        if (!a) return fail(err);
        int idx = rest.isEmpty() ? 0 : a->findPath(rest);
        if (idx < 0) return fail("not found: " + inner);
        if (a->isArchiveEntry(idx)) { a = a->child(idx, &err); if (!a) return fail(err); idx = 0; }
        if (!a->entry(idx).isDir()) { listDir(a, a->entry(idx).parent, false, QString()); return 0; }
        listDir(a, idx, rec, QString());
        return 0;
    }
    if (cmd == "extract" || cmd == "cat") {
        if (!need(cmd == "cat" ? 2 : 3)) return 1;
        QString inner = args[2] == "." ? QString() : args[2];
        QString rest;
        auto a = openChain(args[1], inner, rest, &err);
        if (!a) return fail(err);
        int idx = rest.isEmpty() ? 0 : a->findPath(rest);
        if (idx < 0) return fail("not found: " + inner);
        if (cmd == "cat") {
            QByteArray d;
            if (!a->extract(idx, d, &err)) return fail(err);
            fwrite(d.constData(), 1, size_t(d.size()), stdout);
            return 0;
        }
        int count = 0;
        if (int r = extractEntry(a, idx, args[3], count)) return r;
        out << "Extracted " << count << " file(s) to " << args[3] << Qt::endl;
        return 0;
    }
    if (cmd == "add" || cmd == "delete" || cmd == "mkdir" || cmd == "rename") {
        if (!need(cmd == "add" ? 3 : (cmd == "rename" ? 3 : 2))) return 1;
        QString target = args[2];
        // The archive to modify is the deepest .rpf in the path.
        QString rest;
        auto a = openChain(args[1], target, rest, &err);
        if (!a) return fail(err);
        if (cmd == "add" && !rest.isEmpty()) {
            int ri = a->findPath(rest);
            if (ri >= 0 && a->isArchiveEntry(ri)) { a = a->child(ri, &err); if (!a) return fail(err); rest.clear(); }
        }
        QStringList extra = args.mid(3);
        bool ok = modifyArchive(a, [&](RpfNode& root, QString* e) {
            if (cmd == "add") {
                RpfNode* dir = rest.isEmpty() ? &root : root.ensureDir(rest);
                if (!dir) { *e = "not a folder: " + rest; return false; }
                for (const QString& p : extra) {
                    if (!QFileInfo::exists(p)) { *e = "no such file: " + p; return false; }
                    addPathToNode(*dir, p);
                }
                return true;
            }
            if (cmd == "mkdir") return root.ensureDir(rest) != nullptr;
            RpfNode* n = root.findPath(rest);
            if (!n || !n->parent) { *e = "not found: " + rest; return false; }
            if (cmd == "delete") { n->parent->take(n); return true; }
            if (n->parent->child(extra[0]) && n->parent->child(extra[0]) != n) { *e = "name already exists"; return false; }
            n->name = extra[0];
            return true;
        }, &err);
        if (!ok) return fail(err);
        out << "OK" << Qt::endl;
        return 0;
    }
    if (cmd == "new") {
        if (!need(1)) return 1;
        if (QFileInfo::exists(args[1])) return fail(args[1] + " already exists");
        if (!createEmptyArchive(args[1], &err)) return fail(err);
        out << "Created " << args[1] << Qt::endl;
        return 0;
    }
    if (cmd == "pack") {
        if (!need(2)) return 1;
        auto root = newDirNode();
        QStringList temps;
        bool ok = packFolder(args[1], *root, temps, &err) && writeRpfFile(*root, args[2], &err);
        for (auto& t : temps) QFile::remove(t);
        if (!ok) return fail(err);
        out << "Packed " << args[1] << " -> " << args[2] << Qt::endl;
        return 0;
    }
    if (cmd == "verify") {
        // Extracts every entry (recursively through nested archives) in memory.
        if (!need(1)) return 1;
        auto top = RpfArchive::openFile(args[1], &err);
        if (!top) return fail(err);
        bool verbose = args.contains("-v");
        qint64 files = 0, archives = 0, failures = 0, bytes = 0, encrypted = 0;
        std::map<uint32_t, qint64> resVersions;
        std::map<QString, qint64> extCounts;
        std::function<void(const std::shared_ptr<RpfArchive>&)> walk = [&](const std::shared_ptr<RpfArchive>& a) {
            ++archives;
            for (int i = 0; i < a->count(); ++i) {
                const RpfEntry& e = a->entry(i);
                if (e.isDir()) continue;
                QString where = a->displayPath() + "/" + a->entryPath(i);
                if (a->isArchiveEntry(i)) {
                    QString cerr;
                    auto c = a->child(i, &cerr);
                    if (!c) { ++failures; errs << "FAIL archive " << where << ": " << cerr << Qt::endl; continue; }
                    walk(c);
                    continue;
                }
                ++files;
                if (e.encrypted) ++encrypted;
                if (e.isResource()) resVersions[e.resourceVersion()]++;
                extCounts[QFileInfo(e.name).suffix().toLower()]++;
                QByteArray d;
                QString xerr;
                if (!a->extract(i, d, &xerr)) { ++failures; errs << "FAIL " << where << ": " << xerr << Qt::endl; continue; }
                if (e.isResource()) {
                    ResourceData rd;
                    if (!rd.load(d, &xerr)) { ++failures; errs << "FAIL resource " << where << ": " << xerr << Qt::endl; continue; }
                }
                bytes += d.size();
                if (verbose) out << where << Qt::endl;
            }
        };
        walk(top);
        out << "Archives: " << archives << "  Files: " << files << "  Encrypted: " << encrypted
            << "  Extracted: " << (bytes >> 20) << " MB  Failures: " << failures << "\nResource versions:";
        for (auto& kv : resVersions) out << " v" << kv.first << "=" << kv.second;
        out << "\nTypes:";
        for (auto& kv : extCounts) if (kv.second >= 20) out << " " << (kv.first.isEmpty() ? "(none)" : kv.first) << "=" << kv.second;
        out << Qt::endl;
        return failures ? 2 : 0;
    }
    if (cmd == "defrag") {
        if (!need(1)) return 1;
        auto a = openTarget(args[1], args.value(2), &err);
        if (!a) return fail(err);
        qint64 before = QFileInfo(args[1]).size();
        if (!modifyArchive(a, nullptr, &err, true)) return fail(err);
        out << "Rebuilt. " << before << " -> " << QFileInfo(args[1]).size() << " bytes" << Qt::endl;
        return 0;
    }
    if (cmd == "ytd-list" || cmd == "ytd-export") {
        if (!need(cmd == "ytd-list" ? 1 : 2)) return 1;
        QByteArray d;
        if (!readFile(args[1], d)) return fail("cannot read " + args[1]);
        TextureDictionary td;
        if (!td.load(d, &err)) return fail(err);
        bool png = args.contains("--png");
        for (auto& t : td.textures) {
            if (cmd == "ytd-list") {
                out << QStringLiteral("%1  %2x%3  %4  mips:%5\n").arg(t.name, -32).arg(t.image.width).arg(t.image.height)
                           .arg(texFormatName(t.image.format), -12).arg(t.image.levels);
                continue;
            }
            QString base = QDir(args[2]).filePath(t.name);
            if (png) {
                QImage img = decodeTexture(t.image);
                QDir().mkpath(args[2]);
                if (img.isNull() || !img.save(base + ".png")) errs << "warning: cannot convert " << t.name << Qt::endl;
            } else if (!writeFile(base + ".dds", writeDds(t.image))) return fail("cannot write " + base);
        }
        if (cmd == "ytd-export") out << "Exported " << td.textures.size() << " texture(s)" << Qt::endl;
        out.flush();
        return 0;
    }
    if (cmd == "ytd-build" || cmd == "ytd-replace") {
        if (!need(2)) return 1;
        TextureDictionary td;
        QStringList images = args.mid(2);
        QString forcedName;
        if (cmd == "ytd-replace") {
            QByteArray d;
            if (!readFile(args[1], d) || !td.load(d, &err)) return fail(err.isEmpty() ? "cannot read " + args[1] : err);
            forcedName = args.value(3);
            images = {args[2]};
        }
        for (const QString& p : images) {
            TextureImage t;
            if (!loadImageAsTexture(p, t, &err)) return fail(err);
            td.setTexture(forcedName.isEmpty() ? QFileInfo(p).completeBaseName() : forcedName, t);
        }
        QByteArray o = td.build(&err);
        if (o.isEmpty()) return fail(err);
        if (!writeFile(args[1], o)) return fail("cannot write " + args[1]);
        out << "Wrote " << args[1] << " (" << td.textures.size() << " textures)" << Qt::endl;
        return 0;
    }
    if (cmd == "model-obj") {
        if (!need(2)) return 1;
        QByteArray d;
        if (!readFile(args[1], d)) return fail("cannot read " + args[1]);
        std::vector<DrawableModel3D> models;
        if (!loadDrawables(d, QFileInfo(args[1]).suffix(), models, &err)) return fail(err);
        QString mtl = QFileInfo(args[2]).completeBaseName() + ".mtl";
        QByteArray obj;
        for (auto& m : models) obj += exportObj(m, mtl);
        if (!writeFile(args[2], obj)) return fail("cannot write " + args[2]);
        writeFile(QFileInfo(args[2]).absolutePath() + "/" + mtl, exportMtl(models[0]));
        size_t tris = 0;
        for (auto& m : models) for (auto& mesh : m.meshes) tris += mesh.indices.size() / 3;
        out << "Wrote " << args[2] << ": " << models.size() << " drawable(s), " << tris << " triangles" << Qt::endl;
        return 0;
    }
    if (cmd == "gxt2-dump") {
        if (!need(1)) return 1;
        QByteArray d;
        Gxt2 g;
        if (!readFile(args[1], d) || !g.load(d, &err)) return fail(err.isEmpty() ? "cannot read" : err);
        out << g.toText();
        return 0;
    }
    if (cmd == "gxt2-build") {
        if (!need(2)) return 1;
        QByteArray d;
        Gxt2 g;
        if (!readFile(args[1], d) || !g.fromText(QString::fromUtf8(d), &err)) return fail(err.isEmpty() ? "cannot read" : err);
        if (!writeFile(args[2], g.build())) return fail("cannot write");
        out << "Wrote " << g.entries.size() << " entries" << Qt::endl;
        return 0;
    }
    if (cmd == "rbf2xml") {
        if (!need(1)) return 1;
        QByteArray d;
        QString xml;
        if (!readFile(args[1], d) || !rbfToXml(d, xml, &err)) return fail(err.isEmpty() ? "cannot read" : err);
        out << xml;
        return 0;
    }
    if (cmd == "oiv-info" || cmd == "oiv-install") {
        if (!need(1)) return 1;
        OivPackage pkg;
        if (!pkg.open(args[1], &err)) return fail(err);
        if (cmd == "oiv-info") {
            out << "Name:        " << pkg.name << "\nVersion:     " << pkg.versionText << "\nAuthor:      " << pkg.author
                << "\nFormat:      " << pkg.formatVersion << "\nDescription: " << pkg.description << "\n\nOperations:\n";
            for (auto& op : pkg.describeOperations()) out << "  " << op << "\n";
            out.flush();
            return 0;
        }
        GameFS fs;
        int gi = args.indexOf("--game");
        if (gi < 0 || gi + 1 >= args.size()) return fail("--game <dir> is required");
        fs.gameDir = args[gi + 1];
        fs.useModsFolder = !args.contains("--no-mods");
        OivInstaller inst(fs);
        inst.dryRun = args.contains("--dry-run");
        inst.log = [](const QString& s) { out << s << Qt::endl; };
        if (!inst.install(pkg, &err)) return fail(err);
        return 0;
    }
    usage();
    return fail("unknown command: " + cmd);
}
