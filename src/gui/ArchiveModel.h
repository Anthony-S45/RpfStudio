// Virtual file tree spanning folders on disk and (nested) RPF archives.
#pragma once
#include "Rpf.h"
#include <QAbstractItemModel>
#include <QAbstractTableModel>
#include <QIcon>
#include <memory>
#include <vector>

struct VNode {
    enum Kind { FsDir, FsFile, Archive, ArcDir, ArcFile } kind = FsDir;
    QString name;
    QString fsPath;                          // FsDir, FsFile, disk Archive
    std::shared_ptr<rs::RpfArchive> arc;     // Archive: itself (when open); ArcDir/ArcFile: containing archive
    int entry = -1;                          // ArcDir/ArcFile: entry index; Archive: 0
    std::shared_ptr<rs::RpfArchive> outerArc; // nested Archive: containing archive
    int outerEntry = -1;
    qint64 size = 0;
    QString error;
    VNode* parent = nullptr;
    std::vector<std::unique_ptr<VNode>> children;
    std::vector<VNode*> containers;          // children that are containers (for the tree)
    bool populated = false;

    bool isContainer() const { return kind == FsDir || kind == Archive || kind == ArcDir; }
    bool inArchive() const { return kind == ArcDir || kind == ArcFile || (kind == Archive && outerArc); }
    QString path() const;                    // display path
    QString extension() const;
    int row() const;
    // Archive that holds this node's entries / the archive this node is.
    std::shared_ptr<rs::RpfArchive> archive() const { return arc; }
    // Path relative to the containing archive ("" for an archive root).
    QString innerPath() const;
    VNode* topArchive();                     // outermost archive node above (or this)
};

class VTree {
public:
    VNode* root() { return m_root.get(); }
    void setRoot(const QString& dir);
    void setRootFile(const QString& file);   // a single archive
    bool populate(VNode* n);                 // returns false on error (n->error set)
    void reset(VNode* n);                    // drop children, re-read on next access
    VNode* find(const QString& path);        // by display path (deepest existing node)
    int generation = 0;                      // incremented whenever nodes are created
private:
    std::unique_ptr<VNode> m_root;
};

QIcon iconFor(const VNode* n);
QString typeNameFor(const VNode* n);

class FolderTreeModel : public QAbstractItemModel {
    Q_OBJECT
public:
    explicit FolderTreeModel(VTree* t, QObject* parent = nullptr) : QAbstractItemModel(parent), m_t(t) {}
    QModelIndex index(int row, int column, const QModelIndex& parent) const override;
    QModelIndex parent(const QModelIndex& child) const override;
    int rowCount(const QModelIndex& parent) const override;
    int columnCount(const QModelIndex&) const override { return 1; }
    QVariant data(const QModelIndex& idx, int role) const override;
    bool hasChildren(const QModelIndex& parent) const override;
    bool canFetchMore(const QModelIndex& parent) const override;
    void fetchMore(const QModelIndex& parent) override;
    QModelIndex indexFor(VNode* n) const;
    VNode* node(const QModelIndex& i) const { return i.isValid() ? static_cast<VNode*>(i.internalPointer()) : m_t->root(); }
    void beginReset() { beginResetModel(); }
    void endReset() { endResetModel(); }
private:
    VTree* m_t;
};

class FileListModel : public QAbstractTableModel {
    Q_OBJECT
public:
    explicit FileListModel(VTree* t, QObject* parent = nullptr) : QAbstractTableModel(parent), m_t(t) {}
    void setFolder(VNode* n);
    VNode* folder() const { return m_folder; }
    void setFilter(const QString& f);
    VNode* node(const QModelIndex& i) const { return i.isValid() && i.row() < int(m_items.size()) ? m_items[size_t(i.row())] : nullptr; }
    int rowCount(const QModelIndex& p) const override { return p.isValid() ? 0 : int(m_items.size()); }
    int columnCount(const QModelIndex&) const override { return 4; }
    QVariant data(const QModelIndex& idx, int role) const override;
    QVariant headerData(int section, Qt::Orientation o, int role) const override;
    void sort(int column, Qt::SortOrder order) override;
    Qt::ItemFlags flags(const QModelIndex& i) const override;
    QStringList mimeTypes() const override;
    QMimeData* mimeData(const QModelIndexList& indexes) const override;
    Qt::DropActions supportedDropActions() const override { return Qt::CopyAction; }
    Qt::DropActions supportedDragActions() const override { return Qt::CopyAction; }
    bool canDropMimeData(const QMimeData*, Qt::DropAction, int, int, const QModelIndex&) const override;
    bool dropMimeData(const QMimeData* data, Qt::DropAction action, int row, int column, const QModelIndex& parent) override;
    void clear();
signals:
    void filesDropped(const QStringList& paths);
private:
    void rebuild();
    VTree* m_t;
    VNode* m_folder = nullptr;
    QString m_filter;
    std::vector<VNode*> m_items;
    int m_sortCol = 0;
    Qt::SortOrder m_sortOrder = Qt::AscendingOrder;
};

QString formatSize(qint64 s);
// Extracts a node (file or folder, recursively) to a directory on disk.
bool exportNode(VNode* n, const QString& dir, QString* err, int* count = nullptr);
