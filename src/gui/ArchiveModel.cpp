#include "ArchiveModel.h"
#include "Resource.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMimeData>
#include <QPainter>
#include <QRegularExpression>
#include <QSet>
#include <QStyle>
#include <QTemporaryDir>
#include <QUrl>
#include <algorithm>

// ------------------------------------------------------------------ VNode

QString VNode::path() const
{
    if (!parent) return fsPath;
    return parent->path() + "/" + name;
}

QString VNode::extension() const
{
    int dot = name.lastIndexOf('.');
    return dot < 0 ? QString() : name.mid(dot + 1).toLower();
}

int VNode::row() const
{
    if (!parent) return 0;
    auto& c = parent->containers;
    auto it = std::find(c.begin(), c.end(), this);
    return it == c.end() ? 0 : int(it - c.begin());
}

QString VNode::innerPath() const
{
    if ((kind == ArcDir || kind == ArcFile) && arc) return arc->entryPath(entry);
    return QString();
}

VNode* VNode::topArchive()
{
    VNode* top = nullptr;
    for (VNode* n = this; n; n = n->parent)
        if (n->kind == Archive) top = n;
    return top;
}

// ------------------------------------------------------------------ VTree

void VTree::setRoot(const QString& dir)
{
    m_root = std::make_unique<VNode>();
    m_root->kind = VNode::FsDir;
    m_root->fsPath = QDir(dir).absolutePath();
    m_root->name = QFileInfo(dir).fileName();
}

void VTree::setRootFile(const QString& file)
{
    setRoot(QFileInfo(file).absolutePath());
}

static std::unique_ptr<VNode> makeArcChild(const std::shared_ptr<rs::RpfArchive>& a, int c, VNode* parent)
{
    const rs::RpfEntry& e = a->entry(c);
    auto n = std::make_unique<VNode>();
    n->parent = parent;
    n->name = e.name;
    if (e.isDir()) {
        n->kind = VNode::ArcDir;
        n->arc = a;
        n->entry = c;
    } else if (a->isArchiveEntry(c) && !e.isCompressed()) {
        n->kind = VNode::Archive;
        n->outerArc = a;
        n->outerEntry = c;
        n->entry = 0;
        n->size = e.fileSize();
    } else {
        n->kind = VNode::ArcFile;
        n->arc = a;
        n->entry = c;
        n->size = e.fileSize();
    }
    return n;
}

bool VTree::populate(VNode* n)
{
    if (n->populated) return n->error.isEmpty();
    n->populated = true;
    ++generation;
    n->children.clear();
    n->containers.clear();
    n->error.clear();
    std::vector<std::unique_ptr<VNode>> kids;
    if (n->kind == VNode::FsDir) {
        QDir d(n->fsPath);
        const auto list = d.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot, QDir::DirsFirst | QDir::Name | QDir::IgnoreCase);
        for (const QFileInfo& fi : list) {
            auto c = std::make_unique<VNode>();
            c->parent = n;
            c->name = fi.fileName();
            c->fsPath = fi.absoluteFilePath();
            c->size = fi.size();
            if (fi.isDir()) c->kind = VNode::FsDir;
            else if (fi.suffix().compare("rpf", Qt::CaseInsensitive) == 0) { c->kind = VNode::Archive; c->entry = 0; }
            else c->kind = VNode::FsFile;
            kids.push_back(std::move(c));
        }
    } else if (n->kind == VNode::Archive || n->kind == VNode::ArcDir) {
        if (n->kind == VNode::Archive && !n->arc) {
            QString err;
            n->arc = n->outerArc ? n->outerArc->child(n->outerEntry, &err) : rs::RpfArchive::openFile(n->fsPath, &err);
            if (!n->arc) { n->error = err; return false; }
        }
        int dir = n->kind == VNode::Archive ? 0 : n->entry;
        for (int c : n->arc->children(dir)) kids.push_back(makeArcChild(n->arc, c, n));
        std::stable_sort(kids.begin(), kids.end(), [](const std::unique_ptr<VNode>& a, const std::unique_ptr<VNode>& b) {
            bool ad = a->kind == VNode::ArcDir, bd = b->kind == VNode::ArcDir;
            if (ad != bd) return ad;
            return a->name.compare(b->name, Qt::CaseInsensitive) < 0;
        });
    }
    n->children = std::move(kids);
    for (auto& c : n->children)
        if (c->isContainer()) n->containers.push_back(c.get());
    return true;
}

void VTree::reset(VNode* n)
{
    n->children.clear();
    n->containers.clear();
    n->populated = false;
    n->error.clear();
    if (n->kind == VNode::Archive) {
        n->arc.reset();
        if (!n->outerArc && !n->fsPath.isEmpty()) n->size = QFileInfo(n->fsPath).size();
    }
}

VNode* VTree::find(const QString& path)
{
    VNode* cur = m_root.get();
    if (!cur) return nullptr;
    QString rootPath = cur->fsPath;
    if (!path.startsWith(rootPath)) return nullptr;
    QString rest = path.mid(rootPath.size());
    for (const QString& part : rest.split('/', Qt::SkipEmptyParts)) {
        populate(cur);
        VNode* next = nullptr;
        for (auto& c : cur->children)
            if (c->name.compare(part, Qt::CaseInsensitive) == 0) { next = c.get(); break; }
        if (!next) return cur;
        cur = next;
    }
    return cur;
}

// ------------------------------------------------------------------ icons/types

static QIcon badge(const QString& text, const QColor& color)
{
    static QHash<QString, QIcon> cache;
    QString key = text + color.name();
    auto it = cache.find(key);
    if (it != cache.end()) return *it;
    QIcon icon;
    for (int sz : {16, 24, 32}) {
        QPixmap pm(sz, sz);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        QRectF r(1, sz * 0.12, sz - 2, sz * 0.76);
        p.setBrush(color);
        p.setPen(color.darker(140));
        p.drawRoundedRect(r, sz * 0.15, sz * 0.15);
        QFont f = QApplication::font();
        f.setBold(true);
        f.setPixelSize(int(sz * (text.size() > 3 ? 0.30 : 0.36)));
        p.setFont(f);
        p.setPen(Qt::white);
        p.drawText(r, Qt::AlignCenter, text);
        icon.addPixmap(pm);
    }
    cache.insert(key, icon);
    return icon;
}

QIcon iconFor(const VNode* n)
{
    QStyle* st = QApplication::style();
    switch (n->kind) {
    case VNode::FsDir: return st->standardIcon(QStyle::SP_DirIcon);
    case VNode::ArcDir: return st->standardIcon(QStyle::SP_DirIcon);
    case VNode::Archive: return badge("RPF", n->error.isEmpty() ? QColor(150, 95, 40) : QColor(170, 40, 40));
    default: break;
    }
    QString e = n->extension();
    if (e == "ytd") return badge("YTD", QColor(200, 60, 140));
    if (e == "ydr" || e == "ydd" || e == "yft") return badge(e.toUpper(), QColor(50, 110, 200));
    if (e == "ybn") return badge("YBN", QColor(90, 90, 160));
    if (e == "xml" || e == "meta" || e == "txt" || e == "dat" || e == "ide" || e == "ipl" || e == "cfg" || e == "ini" || e == "log")
        return badge(e.left(3).toUpper(), QColor(100, 120, 100));
    if (e == "gxt2") return badge("GXT", QColor(160, 120, 30));
    if (e == "awc") return badge("AWC", QColor(30, 140, 140));
    if (e == "ysc") return badge("YSC", QColor(120, 60, 160));
    if (e == "ymap" || e == "ytyp" || e == "ymt" || e == "ymf") return badge(e.left(4).toUpper(), QColor(70, 140, 70));
    return st->standardIcon(QStyle::SP_FileIcon);
}

QString typeNameFor(const VNode* n)
{
    switch (n->kind) {
    case VNode::FsDir: case VNode::ArcDir: return "Folder";
    case VNode::Archive: return "Archive";
    default: return rs::resourceTypeName(n->extension());
    }
}

QString formatSize(qint64 s)
{
    if (s < 1024) return QString::number(s) + " B";
    if (s < 1024 * 1024) return QString::number(s / 1024.0, 'f', 1) + " KB";
    if (s < 1024LL * 1024 * 1024) return QString::number(s / 1048576.0, 'f', 1) + " MB";
    return QString::number(s / 1073741824.0, 'f', 2) + " GB";
}

// ------------------------------------------------------------------ tree model

QModelIndex FolderTreeModel::index(int row, int column, const QModelIndex& parent) const
{
    VNode* p = node(parent);
    if (!p || row < 0 || row >= int(p->containers.size())) return {};
    return createIndex(row, column, p->containers[size_t(row)]);
}

QModelIndex FolderTreeModel::parent(const QModelIndex& child) const
{
    if (!child.isValid()) return {};
    VNode* n = node(child);
    VNode* p = n->parent;
    if (!p || p == m_t->root()) return {};
    return createIndex(p->row(), 0, p);
}

int FolderTreeModel::rowCount(const QModelIndex& parent) const
{
    VNode* p = node(parent);
    return p ? int(p->containers.size()) : 0;
}

bool FolderTreeModel::hasChildren(const QModelIndex& parent) const
{
    VNode* p = node(parent);
    if (!p) return false;
    if (!p->populated) return p->isContainer();
    return !p->containers.empty();
}

bool FolderTreeModel::canFetchMore(const QModelIndex& parent) const
{
    VNode* p = node(parent);
    return p && !p->populated && p->isContainer();
}

void FolderTreeModel::fetchMore(const QModelIndex& parent)
{
    VNode* p = node(parent);
    if (!p || p->populated) return;
    // populate first, then announce the rows
    std::vector<std::unique_ptr<VNode>> dummy;
    m_t->populate(p);
    int n = int(p->containers.size());
    if (n > 0) {
        beginInsertRows(parent, 0, n - 1);
        endInsertRows();
    }
    if (!p->error.isEmpty()) emit dataChanged(parent, parent);
}

QVariant FolderTreeModel::data(const QModelIndex& idx, int role) const
{
    VNode* n = node(idx);
    if (!n) return {};
    if (role == Qt::DisplayRole) return n->name;
    if (role == Qt::DecorationRole) return iconFor(n);
    if (role == Qt::ToolTipRole) return n->error.isEmpty() ? n->path() : n->error;
    return {};
}

QModelIndex FolderTreeModel::indexFor(VNode* n) const
{
    if (!n || n == m_t->root() || !n->parent) return {};
    return createIndex(n->row(), 0, n);
}

// ------------------------------------------------------------------ list model

void FileListModel::clear()
{
    beginResetModel();
    m_folder = nullptr;
    m_items.clear();
    endResetModel();
}

void FileListModel::setFolder(VNode* n)
{
    beginResetModel();
    m_folder = n;
    if (n) m_t->populate(n);
    rebuild();
    endResetModel();
}

void FileListModel::setFilter(const QString& f)
{
    beginResetModel();
    m_filter = f;
    rebuild();
    endResetModel();
}

void FileListModel::rebuild()
{
    m_items.clear();
    if (!m_folder) return;
    QRegularExpression rx;
    bool wild = m_filter.contains('*') || m_filter.contains('?');
    if (wild) rx = QRegularExpression(QRegularExpression::wildcardToRegularExpression(m_filter), QRegularExpression::CaseInsensitiveOption);
    for (auto& c : m_folder->children) {
        if (!m_filter.isEmpty()) {
            if (wild ? !rx.match(c->name).hasMatch() : !c->name.contains(m_filter, Qt::CaseInsensitive)) continue;
        }
        m_items.push_back(c.get());
    }
    auto col = m_sortCol;
    bool asc = m_sortOrder == Qt::AscendingOrder;
    std::stable_sort(m_items.begin(), m_items.end(), [col, asc](VNode* a, VNode* b) {
        bool ad = a->isContainer() && a->kind != VNode::Archive, bd = b->isContainer() && b->kind != VNode::Archive;
        if (ad != bd) return ad;
        int c = 0;
        if (col == 1) c = typeNameFor(a).compare(typeNameFor(b), Qt::CaseInsensitive);
        else if (col == 2) c = a->size < b->size ? -1 : (a->size > b->size ? 1 : 0);
        if (c == 0) c = a->name.compare(b->name, Qt::CaseInsensitive);
        return asc ? c < 0 : c > 0;
    });
}

void FileListModel::sort(int column, Qt::SortOrder order)
{
    beginResetModel();
    m_sortCol = column;
    m_sortOrder = order;
    rebuild();
    endResetModel();
}

QVariant FileListModel::data(const QModelIndex& idx, int role) const
{
    VNode* n = node(idx);
    if (!n) return {};
    if (role == Qt::DisplayRole) {
        switch (idx.column()) {
        case 0: return n->name;
        case 1: return typeNameFor(n);
        case 2: return (n->kind == VNode::FsDir || n->kind == VNode::ArcDir) ? QVariant() : QVariant(formatSize(n->size));
        case 3: {
            if (n->kind == VNode::ArcFile && n->arc) {
                const rs::RpfEntry& e = n->arc->entry(n->entry);
                QStringList a;
                if (e.isResource()) a << QStringLiteral("resource v%1").arg(e.resourceVersion());
                if (e.isCompressed()) a << "compressed";
                if (e.encrypted) a << "encrypted";
                return a.join(", ");
            }
            if (n->kind == VNode::Archive && n->arc) return rs::encryptionName(n->arc->encryption());
            return {};
        }
        }
    }
    if (role == Qt::DecorationRole && idx.column() == 0) return iconFor(n);
    if (role == Qt::TextAlignmentRole && idx.column() == 2) return int(Qt::AlignRight | Qt::AlignVCenter);
    if (role == Qt::ToolTipRole) return n->error.isEmpty() ? n->path() : n->error;
    return {};
}

QVariant FileListModel::headerData(int section, Qt::Orientation o, int role) const
{
    if (o != Qt::Horizontal || role != Qt::DisplayRole) return {};
    static const char* names[] = {"Name", "Type", "Size", "Attributes"};
    return names[section];
}

Qt::ItemFlags FileListModel::flags(const QModelIndex& i) const
{
    Qt::ItemFlags f = QAbstractTableModel::flags(i) | Qt::ItemIsDropEnabled;
    if (i.isValid()) f |= Qt::ItemIsDragEnabled;
    return f;
}

QStringList FileListModel::mimeTypes() const { return {"text/uri-list"}; }

QMimeData* FileListModel::mimeData(const QModelIndexList& indexes) const
{
    static std::vector<std::unique_ptr<QTemporaryDir>> dirs;
    dirs.push_back(std::make_unique<QTemporaryDir>());
    QString dir = dirs.back()->path();
    QList<QUrl> urls;
    QSet<VNode*> done;
    for (const QModelIndex& i : indexes) {
        VNode* n = node(i);
        if (!n || done.contains(n)) continue;
        done.insert(n);
        QString err;
        if (exportNode(n, dir, &err)) urls << QUrl::fromLocalFile(QDir(dir).filePath(n->name));
    }
    auto* m = new QMimeData;
    m->setUrls(urls);
    return m;
}

bool FileListModel::canDropMimeData(const QMimeData* data, Qt::DropAction, int, int, const QModelIndex&) const
{
    return data->hasUrls() && m_folder;
}

bool FileListModel::dropMimeData(const QMimeData* data, Qt::DropAction, int, int, const QModelIndex&)
{
    QStringList paths;
    for (const QUrl& u : data->urls())
        if (u.isLocalFile()) paths << u.toLocalFile();
    // ignore drops of our own drag (files from our temp dirs into the same folder)
    if (!paths.isEmpty()) emit filesDropped(paths);
    return !paths.isEmpty();
}

// ------------------------------------------------------------------ export

static bool exportArchiveDir(const std::shared_ptr<rs::RpfArchive>& a, int dir, const QString& out, QString* err, int* count)
{
    QDir().mkpath(out);
    for (int c : a->children(dir)) {
        const rs::RpfEntry& e = a->entry(c);
        QString p = QDir(out).filePath(e.name);
        if (e.isDir()) {
            if (!exportArchiveDir(a, c, p, err, count)) return false;
            continue;
        }
        QByteArray d;
        if (!a->extract(c, d, err)) return false;
        QFile f(p);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate) || f.write(d) != d.size()) { if (err) *err = "Cannot write " + p; return false; }
        if (count) ++*count;
    }
    return true;
}

bool exportNode(VNode* n, const QString& dir, QString* err, int* count)
{
    QDir().mkpath(dir);
    QString target = QDir(dir).filePath(n->name);
    switch (n->kind) {
    case VNode::FsFile:
        QFile::remove(target);
        if (!QFile::copy(n->fsPath, target)) { if (err) *err = "Cannot copy " + n->fsPath; return false; }
        if (count) ++*count;
        return true;
    case VNode::FsDir: {
        QDir().mkpath(target);
        for (const QFileInfo& fi : QDir(n->fsPath).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot)) {
            VNode tmp;
            tmp.kind = fi.isDir() ? VNode::FsDir : VNode::FsFile;
            tmp.fsPath = fi.absoluteFilePath();
            tmp.name = fi.fileName();
            if (!exportNode(&tmp, target, err, count)) return false;
        }
        return true;
    }
    case VNode::ArcFile: {
        QByteArray d;
        if (!n->arc->extract(n->entry, d, err)) return false;
        QFile f(target);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate) || f.write(d) != d.size()) { if (err) *err = "Cannot write " + target; return false; }
        if (count) ++*count;
        return true;
    }
    case VNode::ArcDir:
        return exportArchiveDir(n->arc, n->entry, target, err, count);
    case VNode::Archive: {
        // archives are exported as files (the .rpf itself)
        if (n->outerArc) {
            QByteArray d = n->outerArc->readStored(n->outerEntry);
            QFile f(target);
            if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate) || f.write(d) != d.size()) { if (err) *err = "Cannot write " + target; return false; }
        } else {
            QFile::remove(target);
            if (!QFile::copy(n->fsPath, target)) { if (err) *err = "Cannot copy " + n->fsPath; return false; }
        }
        if (count) ++*count;
        return true;
    }
    }
    return false;
}
