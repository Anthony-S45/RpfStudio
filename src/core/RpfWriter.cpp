#include "RpfWriter.h"
#include "BinaryIO.h"
#include "Compression.h"
#include "Crypto.h"

#include <QDir>
#include <QFileInfo>
#include <QSaveFile>
#include <QTemporaryFile>
#include <algorithm>
#include <cstdio>
#include <deque>
#include <map>

namespace rs {

// ------------------------------------------------------------------ tree

RpfNode* RpfNode::child(const QString& n) const
{
    for (auto& c : children)
        if (c->name.compare(n, Qt::CaseInsensitive) == 0) return c.get();
    return nullptr;
}

RpfNode* RpfNode::findPath(const QString& rel)
{
    QString p = rel;
    p.replace('\\', '/');
    RpfNode* cur = this;
    for (const QString& part : p.split('/', Qt::SkipEmptyParts)) {
        if (!cur->isDir()) return nullptr;
        cur = cur->child(part);
        if (!cur) return nullptr;
    }
    return cur;
}

RpfNode* RpfNode::ensureDir(const QString& rel)
{
    QString p = rel;
    p.replace('\\', '/');
    RpfNode* cur = this;
    for (const QString& part : p.split('/', Qt::SkipEmptyParts)) {
        RpfNode* c = cur->child(part);
        if (!c) c = cur->addChild(newDirNode(part));
        if (!c->isDir()) return nullptr;
        cur = c;
    }
    return cur;
}

RpfNode* RpfNode::addChild(std::unique_ptr<RpfNode> n)
{
    for (auto& c : children) {
        if (c->name.compare(n->name, Qt::CaseInsensitive) == 0) {
            n->parent = this;
            c = std::move(n);
            return c.get();
        }
    }
    n->parent = this;
    children.push_back(std::move(n));
    return children.back().get();
}

std::unique_ptr<RpfNode> RpfNode::take(RpfNode* c)
{
    for (auto it = children.begin(); it != children.end(); ++it) {
        if (it->get() == c) {
            auto n = std::move(*it);
            children.erase(it);
            n->parent = nullptr;
            return n;
        }
    }
    return nullptr;
}

QString RpfNode::path() const
{
    QStringList parts;
    for (const RpfNode* n = this; n && n->parent; n = n->parent) parts.prepend(n->name);
    return parts.join('/');
}

std::unique_ptr<RpfNode> newDirNode(const QString& name)
{
    auto n = std::make_unique<RpfNode>();
    n->name = name;
    n->source = RpfNode::Directory;
    return n;
}

std::unique_ptr<RpfNode> newFileNode(const QString& name, const QByteArray& bytes)
{
    auto n = std::make_unique<RpfNode>();
    n->name = name;
    n->source = RpfNode::FromBytes;
    n->bytes = bytes;
    return n;
}

std::unique_ptr<RpfNode> newDiskFileNode(const QString& name, const QString& path)
{
    auto n = std::make_unique<RpfNode>();
    n->name = name;
    n->source = RpfNode::FromDiskFile;
    n->diskPath = path;
    return n;
}

static void buildTree(const std::shared_ptr<RpfArchive>& a, int dir, RpfNode& node)
{
    for (int c : a->children(dir)) {
        const RpfEntry& e = a->entry(c);
        auto n = std::make_unique<RpfNode>();
        n->name = e.name;
        n->parent = &node;
        if (e.isDir()) {
            n->source = RpfNode::Directory;
            buildTree(a, c, *n);
        } else {
            n->source = RpfNode::FromEntry;
            n->srcArchive = a;
            n->srcEntry = c;
        }
        node.children.push_back(std::move(n));
    }
}

std::unique_ptr<RpfNode> treeFromArchive(const std::shared_ptr<RpfArchive>& a)
{
    auto root = newDirNode();
    buildTree(a, 0, *root);
    return root;
}

// ------------------------------------------------------------------ payloads

namespace {

struct Payload {
    enum Kind { None, Bytes, SrcRange, DiskFile } kind = None;
    QByteArray data;
    std::shared_ptr<FileSource> src;
    qint64 offset = 0, len = 0;
    QString path;
    qint64 length() const { return kind == Bytes ? data.size() : len; }
};

struct Flat {
    RpfNode* node;
    RpfEntry e;
    Payload p;
    uint32_t nameOffset = 0;
};

bool isStoredType(const QString& name)
{
    return name.endsWith(QLatin1String(".rpf"), Qt::CaseInsensitive) ||
           name.endsWith(QLatin1String(".awc"), Qt::CaseInsensitive);
}

bool classifyBytes(const QString& name, QByteArray bytes, bool compress, RpfEntry& e, Payload& p, QString* err)
{
    const qint64 len = bytes.size();
    if (len >= 16 && rd32(bytes.constData()) == Rsc7Magic) {
        e.type = RpfEntry::Resource;
        e.systemFlags = rd32(bytes.constData() + 8);
        e.graphicsFlags = rd32(bytes.constData() + 12);
        e.sizeField = uint32_t(len);
        e.encrypted = name.endsWith(QLatin1String(".ysc"), Qt::CaseInsensitive);
        if (e.encrypted) {
            if (err) *err = name + ": importing script (.ysc) resources is not supported (they require NG encryption)";
            return false;
        }
        if (len >= 0xFFFFFF) { // huge resources keep their size in the header
            uint8_t* d = reinterpret_cast<uint8_t*>(bytes.data());
            d[7] = uint8_t(len);
            d[14] = uint8_t(len >> 8);
            d[5] = uint8_t(len >> 16);
            d[2] = uint8_t(len >> 24);
        }
        p.kind = Payload::Bytes;
        p.data = bytes;
        return true;
    }
    if (len > 0xFFFFFFFFLL) {
        if (err) *err = name + ": file too large";
        return false;
    }
    e.type = RpfEntry::Binary;
    e.encrypted = false;
    e.uncompressedSize = uint32_t(len);
    p.kind = Payload::Bytes;
    if (compress && !isStoredType(name) && len > 0) {
        QByteArray c = deflateRaw(bytes);
        if (!c.isEmpty() && c.size() <= 0xFFFFFF) {
            e.sizeField = uint32_t(c.size());
            p.data = c;
            return true;
        }
    }
    e.sizeField = 0;
    p.data = bytes;
    return true;
}

bool preparePayload(RpfNode* n, RpfEntry& e, Payload& p, QString* err)
{
    switch (n->source) {
    case RpfNode::FromEntry: {
        const RpfEntry& se = n->srcArchive->entry(n->srcEntry);
        e = se;
        e.name = n->name;
        if (se.type == RpfEntry::Binary && se.encrypted && n->srcArchive->encryption() == RpfEncAes) {
            // An OPEN archive implies NG for encrypted files, so AES content must be decrypted.
            QByteArray d = n->srcArchive->readStored(n->srcEntry);
            if (!GameKeys::global().hasAes()) { if (err) *err = n->name + ": AES key required"; return false; }
            aesDecrypt(d, GameKeys::global().aesKey);
            e.encrypted = false;
            p.kind = Payload::Bytes;
            p.data = d;
            return true;
        }
        if (n->name.compare(se.name, Qt::CaseInsensitive) != 0 && se.encrypted) {
            if (err) *err = n->name + ": encrypted files cannot be renamed (the key depends on the name)";
            return false;
        }
        p.kind = Payload::SrcRange;
        p.src = n->srcArchive->source();
        p.offset = n->srcArchive->baseOffset() + qint64(se.offsetBlocks) * 512;
        p.len = se.storedSize();
        return true;
    }
    case RpfNode::FromBytes:
        e.name = n->name;
        return classifyBytes(n->name, n->bytes, n->compress, e, p, err);
    case RpfNode::FromDiskFile: {
        e.name = n->name;
        QFile f(n->diskPath);
        if (!f.open(QIODevice::ReadOnly)) { if (err) *err = "Cannot read " + n->diskPath; return false; }
        QByteArray head = f.peek(16);
        bool isRes = head.size() == 16 && rd32(head.constData()) == Rsc7Magic;
        if (!isRes && (isStoredType(n->name) || !n->compress)) {
            if (f.size() > 0xFFFFFFFFLL) { if (err) *err = n->name + ": file too large"; return false; }
            e.type = RpfEntry::Binary;
            e.sizeField = 0;
            e.uncompressedSize = uint32_t(f.size());
            e.encrypted = false;
            p.kind = Payload::DiskFile;
            p.path = n->diskPath;
            p.len = f.size();
            return true;
        }
        return classifyBytes(n->name, f.readAll(), n->compress, e, p, err);
    }
    case RpfNode::Directory:
        break;
    }
    return true;
}

bool flatten(RpfNode& root, std::vector<Flat>& out, QByteArray& names, QString* err)
{
    out.clear();
    out.push_back({&root, {}, {}, 0});
    out[0].e.type = RpfEntry::Directory;
    std::deque<size_t> queue{0};
    while (!queue.empty()) {
        size_t di = queue.front();
        queue.pop_front();
        RpfNode* d = out[di].node;
        std::vector<RpfNode*> kids;
        for (auto& c : d->children) kids.push_back(c.get());
        // The game binary-searches directory listings, so entries must be sorted.
        std::sort(kids.begin(), kids.end(), [](RpfNode* a, RpfNode* b) {
            return a->name.toLower().toUtf8() < b->name.toLower().toUtf8();
        });
        for (size_t i = 1; i < kids.size(); ++i) {
            if (kids[i]->name.compare(kids[i - 1]->name, Qt::CaseInsensitive) == 0) {
                if (err) *err = "Duplicate entry name: " + kids[i]->path();
                return false;
            }
        }
        out[di].e.entriesIndex = uint32_t(out.size());
        out[di].e.entriesCount = uint32_t(kids.size());
        for (RpfNode* k : kids) {
            Flat f{k, {}, {}, 0};
            if (k->isDir()) f.e.type = RpfEntry::Directory;
            out.push_back(std::move(f));
            if (k->isDir()) queue.push_back(out.size() - 1);
        }
    }

    names.clear();
    std::map<QByteArray, uint32_t> dedupe;
    for (Flat& f : out) {
        QByteArray n = (f.node == &root) ? QByteArray() : f.node->name.toLower().toUtf8();
        auto it = dedupe.find(n);
        if (it != dedupe.end()) { f.nameOffset = it->second; continue; }
        f.nameOffset = uint32_t(names.size());
        dedupe[n] = f.nameOffset;
        names.append(n);
        names.append('\0');
    }
    while (names.size() % 16) names.append('\0');
    for (Flat& f : out) {
        if (f.e.type != RpfEntry::Directory && f.nameOffset > 0xFFFF) {
            if (err) *err = "Too many / too long file names for one archive (name table exceeds 64 KB)";
            return false;
        }
    }
    return true;
}

QByteArray buildHeader(const std::vector<Flat>& flat, const QByteArray& names)
{
    Writer w;
    w.u32(RpfMagic);
    w.u32(uint32_t(flat.size()));
    w.u32(uint32_t(names.size()));
    w.u32(RpfEncOpen);
    for (const Flat& f : flat) {
        const RpfEntry& e = f.e;
        if (e.type == RpfEntry::Directory) {
            w.u32(f.nameOffset);
            w.u32(0x7FFFFF00);
            w.u32(e.entriesIndex);
            w.u32(e.entriesCount);
        } else if (e.type == RpfEntry::Binary) {
            w.u16(uint16_t(f.nameOffset));
            w.u8(uint8_t(e.sizeField)); w.u8(uint8_t(e.sizeField >> 8)); w.u8(uint8_t(e.sizeField >> 16));
            w.u8(uint8_t(e.offsetBlocks)); w.u8(uint8_t(e.offsetBlocks >> 8)); w.u8(uint8_t(e.offsetBlocks >> 16));
            w.u32(e.uncompressedSize);
            w.u32(e.encrypted ? 1 : 0);
        } else {
            uint32_t s = std::min<uint32_t>(e.sizeField, 0xFFFFFF);
            w.u16(uint16_t(f.nameOffset));
            w.u8(uint8_t(s)); w.u8(uint8_t(s >> 8)); w.u8(uint8_t(s >> 16));
            w.u8(uint8_t(e.offsetBlocks)); w.u8(uint8_t(e.offsetBlocks >> 8)); w.u8(uint8_t((e.offsetBlocks >> 16) | 0x80));
            w.u32(e.systemFlags);
            w.u32(e.graphicsFlags);
        }
    }
    w.bytes(names);
    return w.data;
}

qint64 blocksFor(qint64 bytes) { return (bytes + 511) / 512; }

bool checkOffset(const Flat& f, qint64 block, QString* err)
{
    qint64 limit = f.e.type == RpfEntry::Resource ? 0x7FFFFF : 0xFFFFFF;
    if (block > limit) {
        if (err) *err = "Archive too large (offset limit reached) at " + f.node->path();
        return false;
    }
    return true;
}

bool writePayload(QIODevice& out, qint64 pos, const Payload& p, QString* err)
{
    if (!out.seek(pos)) { if (err) *err = "seek failed"; return false; }
    const qint64 chunk = 8 << 20;
    switch (p.kind) {
    case Payload::Bytes:
        if (out.write(p.data) != p.data.size()) { if (err) *err = "write failed: " + out.errorString(); return false; }
        break;
    case Payload::SrcRange:
        for (qint64 done = 0; done < p.len;) {
            qint64 n = std::min(chunk, p.len - done);
            QByteArray b = p.src->read(p.offset + done, n);
            if (b.size() != n) { if (err) *err = "read failed from " + p.src->path(); return false; }
            if (out.write(b) != n) { if (err) *err = "write failed: " + out.errorString(); return false; }
            done += n;
        }
        break;
    case Payload::DiskFile: {
        QFile f(p.path);
        if (!f.open(QIODevice::ReadOnly)) { if (err) *err = "Cannot read " + p.path; return false; }
        while (!f.atEnd()) {
            QByteArray b = f.read(chunk);
            if (out.write(b) != b.size()) { if (err) *err = "write failed: " + out.errorString(); return false; }
        }
        break;
    }
    case Payload::None:
        break;
    }
    qint64 len = p.length();
    qint64 padded = blocksFor(len) * 512;
    if (padded > len) out.write(QByteArray(int(padded - len), 0));
    return true;
}

bool prepareAll(std::vector<Flat>& flat, QString* err, const WriteProgress& progress)
{
    size_t done = 0;
    for (Flat& f : flat) {
        ++done;
        if (f.e.type == RpfEntry::Directory) continue;
        if (progress && (done % 500 == 0)) progress(QStringLiteral("Preparing %1/%2").arg(done).arg(flat.size()));
        uint32_t ei = f.e.entriesIndex;
        (void)ei;
        if (!preparePayload(f.node, f.e, f.p, err)) return false;
    }
    return true;
}

} // namespace

bool writeRpf(RpfNode& root, QIODevice& out, QString* err, const WriteProgress& progress)
{
    std::vector<Flat> flat;
    QByteArray names;
    if (!flatten(root, flat, names, err)) return false;
    if (!prepareAll(flat, err, progress)) return false;

    qint64 headerBytes = 16 + qint64(flat.size()) * 16 + names.size();
    qint64 block = blocksFor(headerBytes);
    for (Flat& f : flat) {
        if (f.e.type == RpfEntry::Directory) continue;
        if (!checkOffset(f, block, err)) return false;
        f.e.offsetBlocks = uint32_t(block);
        block += std::max<qint64>(1, blocksFor(f.p.length()));
    }
    QByteArray header = buildHeader(flat, names);
    header.append(QByteArray(int(blocksFor(header.size()) * 512 - header.size()), 0));
    if (!out.seek(0) || out.write(header) != header.size()) { if (err) *err = "write failed"; return false; }
    size_t n = 0;
    for (Flat& f : flat) {
        if (f.e.type == RpfEntry::Directory) continue;
        if (progress && (++n % 200 == 0)) progress(QStringLiteral("Writing %1").arg(f.node->path()));
        if (!writePayload(out, qint64(f.e.offsetBlocks) * 512, f.p, err)) return false;
    }
    return true;
}

bool writeRpfFile(RpfNode& root, const QString& path, QString* err, const WriteProgress& progress)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile f(path);
    f.setDirectWriteFallback(false);
    if (!f.open(QIODevice::WriteOnly)) { if (err) *err = "Cannot write " + path + ": " + f.errorString(); return false; }
    if (!writeRpf(root, f, err, progress)) { f.cancelWriting(); return false; }
    if (!f.commit()) { if (err) *err = "Cannot save " + path + ": " + f.errorString(); return false; }
    return true;
}

bool createEmptyArchive(const QString& path, QString* err)
{
    auto root = newDirNode();
    return writeRpfFile(*root, path, err);
}

// In-place update of a top-level archive: appends new data and rewrites the TOC.
static bool writeInPlace(const std::shared_ptr<RpfArchive>& arc, RpfNode& root, QString* err, const WriteProgress& progress)
{
    std::vector<Flat> flat;
    QByteArray names;
    if (!flatten(root, flat, names, err)) return false;
    if (!prepareAll(flat, err, progress)) return false;

    QFile out(arc->physicalPath());
    if (!out.open(QIODevice::ReadWrite)) {
        if (err) *err = "Cannot open " + arc->physicalPath() + " for writing: " + out.errorString();
        return false;
    }
    const qint64 headerBlocks = blocksFor(16 + qint64(flat.size()) * 16 + names.size());
    qint64 endBlock = blocksFor(out.size());

    // Entries that stay where they are: unchanged data from this very archive.
    std::vector<Flat*> pending;
    for (Flat& f : flat) {
        if (f.e.type == RpfEntry::Directory) continue;
        bool keep = f.node->source == RpfNode::FromEntry && f.node->srcArchive == arc && f.p.kind == Payload::SrcRange;
        if (keep && qint64(f.e.offsetBlocks) >= headerBlocks) {
            endBlock = std::max(endBlock, qint64(f.e.offsetBlocks) + blocksFor(f.p.length()));
            continue;
        }
        pending.push_back(&f);
    }
    for (Flat* f : pending) {
        if (!checkOffset(*f, endBlock, err)) return false;
        if (progress) progress("Writing " + f->node->path());
        if (!writePayload(out, endBlock * 512, f->p, err)) return false;
        f->e.offsetBlocks = uint32_t(endBlock);
        endBlock += std::max<qint64>(1, blocksFor(f->p.length()));
    }
    if (!out.flush()) { if (err) *err = "flush failed: " + out.errorString(); return false; }
    QByteArray header = buildHeader(flat, names);
    header.append(QByteArray(int(headerBlocks * 512 - header.size()), 0));
    if (!out.seek(0) || out.write(header) != header.size() || !out.flush()) {
        if (err) *err = "Writing archive header failed: " + out.errorString();
        return false;
    }
    out.close();
    return true;
}

static bool commitTree(const std::shared_ptr<RpfArchive>& arc, RpfNode& tree, QString* err, bool fullRebuild,
                       const WriteProgress& progress)
{
    if (!arc->parent()) {
        if (fullRebuild) return writeRpfFile(tree, arc->physicalPath(), err, progress);
        return writeInPlace(arc, tree, err, progress);
    }
    // Nested: rebuild this archive to a temporary file next to the top-level archive,
    // then store it in the parent.
    QString dir = QFileInfo(arc->physicalPath()).absolutePath();
    QTemporaryFile tmp(dir + "/.rpfstudio-XXXXXX.tmp");
    if (!tmp.open()) {
        if (err) *err = "Cannot create temporary file in " + dir;
        return false;
    }
    if (progress) progress("Rebuilding " + arc->name());
    if (!writeRpf(tree, tmp, err, progress)) return false;
    tmp.flush();
    auto parent = arc->parent();
    auto parentTree = treeFromArchive(parent);
    RpfNode* node = parentTree->findPath(parent->entryPath(arc->parentEntry()));
    if (!node) { if (err) *err = "internal error: nested archive entry vanished"; return false; }
    node->source = RpfNode::FromDiskFile;
    node->diskPath = tmp.fileName();
    node->srcArchive.reset();
    node->compress = false;
    return commitTree(parent, *parentTree, err, fullRebuild, progress);
}

bool modifyArchive(const std::shared_ptr<RpfArchive>& archive, const RpfMutator& mutator, QString* err,
                   bool fullRebuild, const WriteProgress& progress)
{
    auto tree = treeFromArchive(archive);
    if (mutator && !mutator(*tree, err)) return false;
    return commitTree(archive, *tree, err, fullRebuild, progress);
}

// ------------------------------------------------------------------ edit session

bool readNodeData(const RpfNode* n, QByteArray& out, QString* err)
{
    switch (n->source) {
    case RpfNode::FromEntry: return n->srcArchive->extract(n->srcEntry, out, err);
    case RpfNode::FromBytes: out = n->bytes; return true;
    case RpfNode::FromDiskFile: {
        QFile f(n->diskPath);
        if (!f.open(QIODevice::ReadOnly)) { if (err) *err = "Cannot read " + n->diskPath; return false; }
        out = f.readAll();
        return true;
    }
    case RpfNode::Directory: break;
    }
    if (err) *err = n->name + " is a directory";
    return false;
}

struct ArchiveEditSession::Sub {
    std::shared_ptr<RpfArchive> archive;   // may be null for new archives
    std::unique_ptr<RpfNode> tree;
    RpfNode* ownerTree = nullptr;          // tree containing the archive's node
    QString pathInOwner;
    int depth = 0;
};

ArchiveEditSession::ArchiveEditSession() = default;

ArchiveEditSession::~ArchiveEditSession()
{
    for (auto& t : m_tempFiles) QFile::remove(t);
}

bool ArchiveEditSession::begin(const QString& diskPath, bool createIfMissing, QString* err)
{
    m_path = diskPath;
    m_subs.clear();
    if (QFileInfo::exists(diskPath)) {
        m_archive = RpfArchive::openFile(diskPath, err);
        if (!m_archive) return false;
        m_root = treeFromArchive(m_archive);
        return true;
    }
    if (!createIfMissing) {
        if (err) *err = "Archive not found: " + diskPath;
        return false;
    }
    m_archive.reset();
    m_root = newDirNode();
    return true;
}

RpfNode* ArchiveEditSession::root() { return m_root.get(); }

RpfNode* ArchiveEditSession::openNested(RpfNode* tree, const QString& path, bool create, QString* err)
{
    for (auto& s : m_subs)
        if (s->ownerTree == tree && s->pathInOwner.compare(path, Qt::CaseInsensitive) == 0) return s->tree.get();
    auto sub = std::make_unique<Sub>();
    sub->ownerTree = tree;
    sub->pathInOwner = path;
    for (auto& s : m_subs) if (s->tree.get() == tree) sub->depth = s->depth + 1;
    RpfNode* node = tree->findPath(path);
    if (node && node->isDir()) { if (err) *err = path + " is a directory, not an archive"; return nullptr; }
    if (!node) {
        if (!create) { if (err) *err = "Archive not found: " + path; return nullptr; }
        sub->tree = newDirNode();
    } else if (node->source == RpfNode::FromEntry) {
        sub->archive = node->srcArchive->child(node->srcEntry, err);
        if (!sub->archive) return nullptr;
        sub->tree = treeFromArchive(sub->archive);
    } else {
        QString file = node->diskPath;
        if (node->source == RpfNode::FromBytes) {
            QTemporaryFile t(QFileInfo(m_path).absolutePath() + "/.rpfstudio-XXXXXX.tmp");
            t.setAutoRemove(false);
            if (!t.open() || t.write(node->bytes) != node->bytes.size()) { if (err) *err = "temp file write failed"; return nullptr; }
            file = t.fileName();
            m_tempFiles.push_back(file);
        }
        sub->archive = RpfArchive::openFile(file, err);
        if (!sub->archive) return nullptr;
        sub->tree = treeFromArchive(sub->archive);
    }
    RpfNode* t = sub->tree.get();
    m_subs.push_back(std::move(sub));
    return t;
}

RpfNode* ArchiveEditSession::resolve(const QString& innerPathIn, QString& rest, bool create, QString* err)
{
    QString p = innerPathIn;
    p.replace('\\', '/');
    QStringList parts = p.split('/', Qt::SkipEmptyParts);
    RpfNode* tree = m_root.get();
    QStringList acc;
    for (int i = 0; i < parts.size(); ++i) {
        acc << parts[i];
        bool last = i == parts.size() - 1;
        if (!last && parts[i].endsWith(QLatin1String(".rpf"), Qt::CaseInsensitive)) {
            tree = openNested(tree, acc.join('/'), create, err);
            if (!tree) return nullptr;
            acc.clear();
        }
    }
    rest = acc.join('/');
    return tree;
}

bool ArchiveEditSession::commit(QString* err, const WriteProgress& progress)
{
    // Write nested archives deepest first, storing each in its owner tree.
    std::vector<Sub*> order;
    for (auto& s : m_subs) order.push_back(s.get());
    std::sort(order.begin(), order.end(), [](Sub* a, Sub* b) { return a->depth > b->depth; });
    for (Sub* s : order) {
        QTemporaryFile t(QFileInfo(m_path).absolutePath() + "/.rpfstudio-XXXXXX.tmp");
        t.setAutoRemove(false);
        if (!t.open()) { if (err) *err = "Cannot create temporary file"; return false; }
        m_tempFiles.push_back(t.fileName());
        if (progress) progress("Rebuilding " + s->pathInOwner);
        if (!writeRpf(*s->tree, t, err, progress)) return false;
        t.close();
        QString dir = QFileInfo(s->pathInOwner).path();
        RpfNode* parentDir = (dir == "." || dir.isEmpty()) ? s->ownerTree : s->ownerTree->ensureDir(dir);
        if (!parentDir) { if (err) *err = "Cannot create folder for " + s->pathInOwner; return false; }
        auto n = newDiskFileNode(QFileInfo(s->pathInOwner).fileName(), t.fileName());
        n->compress = false;
        parentDir->addChild(std::move(n));
    }
    if (m_archive) return writeInPlace(m_archive, *m_root, err, progress);
    QDir().mkpath(QFileInfo(m_path).absolutePath());
    return writeRpfFile(*m_root, m_path, err, progress);
}

} // namespace rs

