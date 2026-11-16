#pragma once
#include "ArchiveModel.h"
#include "RpfWriter.h"
#include "SettingsDialog.h"

#include <QMainWindow>
#include <functional>

class QTreeView;
class QTableView;
class QLineEdit;
class QLabel;
class QAction;
class QSplitter;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    void openPath(const QString& path);   // folder, archive, file inside, or .oiv
    void navigate(const QString& path, bool record = true);

private slots:
    void onTreeSelected();
    void onActivated(const QModelIndex& idx);
    void goBack();
    void goForward();
    void goUp();
    void openFolderDialog();
    void openArchiveDialog();
    void newArchive();
    void installPackage();
    void settings();
    void toggleEditMode(bool on);
    void importFiles();
    void importFolder();
    void exportSelected();
    void replaceSelected();
    void deleteSelected();
    void renameSelected();
    void newFolder();
    void defragment();
    void properties();
    void findFiles();
    void contextMenu(const QPoint& p);
    void filesDropped(const QStringList& paths);

private:
    VNode* currentFolder() const;
    QList<VNode*> selectedNodes() const;
    void openNode(VNode* n);
    bool ensureEditMode();
    // Redirects edits of original game archives to the mods folder. May change `container`.
    bool prepareArchiveEdit(VNode*& container);
    bool editContainer(VNode* container, const std::function<bool(rs::RpfNode& dir, QString* err)>& fn, const QString& what);
    bool editFsFolder(VNode* folder, const std::function<bool(const QString& dir, QString* err)>& fn);
    void reloadUnder(VNode* top, const QString& restorePath);
    bool saveFileAt(const QString& displayPath, const QByteArray& data, QString* err);
    void addFilesTo(VNode* container, const QStringList& paths);
    void updateActions();
    void updateKeyStatus();
    void setRootForPath(const QString& path);

    AppSettings m_s;
    VTree m_tree;
    FolderTreeModel* m_treeModel;
    FileListModel* m_listModel;
    QTreeView* m_treeView;
    QTableView* m_listView;
    QLineEdit* m_address;
    QLineEdit* m_filter;
    QLabel* m_keyLabel;
    QLabel* m_editLabel;
    QAction* m_editAct;
    QAction *m_backAct, *m_fwdAct, *m_upAct;
    QList<QAction*> m_editOnly;
    QStringList m_back, m_forward;
    bool m_editMode = false;
    bool m_navigating = false;
};
