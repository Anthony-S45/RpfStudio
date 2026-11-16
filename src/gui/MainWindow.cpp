#include "MainWindow.h"
#include "Crypto.h"
#include "GameFS.h"
#include "OivDialog.h"
#include "Viewers.h"

#include <QStyle>
#include <QAction>
#include <QApplication>
#include <QBuffer>
#include <QClipboard>
#include <QCloseEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QProgressDialog>
#include <QRegularExpression>
#include <QPushButton>
#include <QSplitter>
#include <QStatusBar>
#include <QTableView>
#include <QToolBar>
#include <QTreeView>
#include <QVBoxLayout>

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent)
{
    m_s = AppSettings::load();
    m_s.loadKeys();
    setWindowTitle("RpfStudio");
    resize(1280, 800);

    m_treeModel = new FolderTreeModel(&m_tree, this);
    m_listModel = new FileListModel(&m_tree, this);

    auto* split = new QSplitter(this);
    m_treeView = new QTreeView;
    m_treeView->setModel(m_treeModel);
    m_treeView->setHeaderHidden(true);
    m_treeView->setUniformRowHeights(true);
    m_listView = new QTableView;
    m_listView->setModel(m_listModel);
    m_listView->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_listView->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_listView->setShowGrid(false);
    m_listView->setSortingEnabled(true);
    m_listView->sortByColumn(0, Qt::AscendingOrder);
    m_listView->verticalHeader()->setVisible(false);
    m_listView->verticalHeader()->setDefaultSectionSize(m_listView->fontMetrics().height() + 6);
    m_listView->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_listView->horizontalHeader()->resizeSection(1, 170);
    m_listView->horizontalHeader()->resizeSection(2, 90);
    m_listView->horizontalHeader()->resizeSection(3, 200);
    m_listView->setContextMenuPolicy(Qt::CustomContextMenu);
    m_listView->setDragEnabled(true);
    m_listView->setAcceptDrops(true);
    m_listView->setDropIndicatorShown(true);
    m_listView->setDragDropMode(QAbstractItemView::DragDrop);
    m_listView->setDefaultDropAction(Qt::CopyAction);
    m_listView->setWordWrap(false);
    m_listView->setIconSize(QSize(18, 18));
    split->addWidget(m_treeView);
    split->addWidget(m_listView);
    split->setStretchFactor(1, 3);
    split->setSizes({320, 960});
    setCentralWidget(split);

    // actions
    auto* tb = addToolBar("Navigation");
    tb->setMovable(false);
    tb->setIconSize(QSize(18, 18));
    m_backAct = tb->addAction(style()->standardIcon(QStyle::SP_ArrowBack), "Back", this, &MainWindow::goBack);
    m_backAct->setShortcut(QKeySequence::Back);
    m_fwdAct = tb->addAction(style()->standardIcon(QStyle::SP_ArrowForward), "Forward", this, &MainWindow::goForward);
    m_fwdAct->setShortcut(QKeySequence::Forward);
    m_upAct = tb->addAction(style()->standardIcon(QStyle::SP_ArrowUp), "Up", this, &MainWindow::goUp);
    m_upAct->setShortcut(Qt::ALT | Qt::Key_Up);
    m_address = new QLineEdit;
    tb->addWidget(m_address);
    m_filter = new QLineEdit;
    m_filter->setPlaceholderText("Filter (e.g. *.ytd)");
    m_filter->setClearButtonEnabled(true);
    m_filter->setMaximumWidth(220);
    tb->addWidget(m_filter);
    tb->addSeparator();
    m_editAct = tb->addAction("Edit mode");
    m_editAct->setCheckable(true);
    m_editAct->setToolTip("Enable editing of archives (like OpenIV's Edit mode)");
    tb->addAction("Install package...", this, &MainWindow::installPackage);
    tb->addAction("Settings", this, &MainWindow::settings);

    QMenu* file = menuBar()->addMenu("&File");
    file->addAction("Open game folder", this, [this]() {
        if (m_s.gameDir.isEmpty()) { settings(); return; }
        openPath(m_s.gameDir);
    });
    file->addAction("Open folder...", this, &MainWindow::openFolderDialog, QKeySequence::Open);
    file->addAction("Open archive...", this, &MainWindow::openArchiveDialog);
    file->addSeparator();
    file->addAction("Install package (.oiv)...", this, &MainWindow::installPackage);
    file->addSeparator();
    file->addAction("Quit", qApp, &QApplication::quit, QKeySequence::Quit);

    QMenu* edit = menuBar()->addMenu("&Edit");
    edit->addAction(m_editAct);
    m_editAct->setShortcut(Qt::CTRL | Qt::Key_E);
    edit->addSeparator();
    m_editOnly << edit->addAction("Import files...", this, &MainWindow::importFiles, Qt::CTRL | Qt::Key_I);
    m_editOnly << edit->addAction("Import folder...", this, &MainWindow::importFolder);
    m_editOnly << edit->addAction("Replace...", this, &MainWindow::replaceSelected);
    m_editOnly << edit->addAction("Rename", this, &MainWindow::renameSelected, Qt::Key_F2);
    m_editOnly << edit->addAction("Delete", this, &MainWindow::deleteSelected, QKeySequence::Delete);
    m_editOnly << edit->addAction("New folder...", this, &MainWindow::newFolder, Qt::CTRL | Qt::SHIFT | Qt::Key_N);
    m_editOnly << edit->addAction("New archive...", this, &MainWindow::newArchive);
    m_editOnly << edit->addAction("Defragment archive", this, &MainWindow::defragment);
    edit->addSeparator();
    edit->addAction("Export selected...", this, &MainWindow::exportSelected, Qt::CTRL | Qt::Key_S);

    QMenu* view = menuBar()->addMenu("&View");
    view->addAction("Refresh", this, [this]() {
        VNode* f = currentFolder();
        if (!f) return;
        VNode* top = f->topArchive();
        reloadUnder(top ? top : f, f->path());
    }, QKeySequence::Refresh);
    view->addAction("Properties", this, &MainWindow::properties, Qt::ALT | Qt::Key_Return);
    view->addAction("Find files...", this, &MainWindow::findFiles, QKeySequence::Find);

    QMenu* tools = menuBar()->addMenu("&Tools");
    tools->addAction("Settings...", this, &MainWindow::settings);

    QMenu* help = menuBar()->addMenu("&Help");
    help->addAction("About", this, [this]() {
        QMessageBox::about(this, "About RpfStudio",
            "<h3>RpfStudio 0.1</h3><p>A GTA V archive explorer and modding tool for Linux.</p>"
            "<p>Not affiliated with Rockstar Games or the OpenIV team. Encryption keys are read from your own copy of the game.</p>"
            "<p>Format knowledge is based on the open-source CodeWalker and RageLib projects.</p>");
    });

    m_keyLabel = new QLabel;
    m_editLabel = new QLabel;
    statusBar()->addPermanentWidget(m_editLabel);
    statusBar()->addPermanentWidget(m_keyLabel);

    connect(m_treeView->selectionModel(), &QItemSelectionModel::currentChanged, this, &MainWindow::onTreeSelected);
    connect(m_listView, &QTableView::activated, this, &MainWindow::onActivated);
    connect(m_listView, &QTableView::customContextMenuRequested, this, &MainWindow::contextMenu);
    connect(m_listModel, &FileListModel::filesDropped, this, &MainWindow::filesDropped);
    connect(m_address, &QLineEdit::returnPressed, this, [this]() { navigate(m_address->text()); });
    connect(m_filter, &QLineEdit::textChanged, m_listModel, &FileListModel::setFilter);
    connect(m_editAct, &QAction::toggled, this, &MainWindow::toggleEditMode);
    connect(m_listView->selectionModel(), &QItemSelectionModel::selectionChanged, this, [this]() {
        auto sel = selectedNodes();
        qint64 total = 0;
        for (auto* n : sel) total += n->size;
        if (!sel.isEmpty()) statusBar()->showMessage(QStringLiteral("%1 selected (%2)").arg(sel.size()).arg(formatSize(total)));
    });

    updateKeyStatus();
    toggleEditMode(false);
    if (!m_s.gameDir.isEmpty() && QDir(m_s.gameDir).exists()) openPath(m_s.gameDir);
    else openPath(QDir::homePath());
}

// ------------------------------------------------------------------ navigation

void MainWindow::updateKeyStatus()
{
    const rs::GameKeys& k = rs::GameKeys::global();
    if (k.hasNg() && k.hasAes()) m_keyLabel->setText("Keys: loaded");
    else m_keyLabel->setText("<span style='color:#c33'>Keys: missing (Settings)</span>");
    m_keyLabel->setToolTip(k.summary());
}

void MainWindow::setRootForPath(const QString& pathIn)
{
    QString path = QDir::cleanPath(pathIn);
    QString game = m_s.gameDir.isEmpty() ? QString() : QDir(m_s.gameDir).absolutePath();
    QString root;
    if (!game.isEmpty() && (path == game || path.startsWith(game + "/"))) root = game;
    else {
        // longest existing directory prefix that is not an archive
        QString p = path;
        while (!p.isEmpty() && !(QFileInfo(p).isDir())) p = QFileInfo(p).path();
        root = p.isEmpty() ? QDir::rootPath() : p;
        if (QFileInfo(path).isDir()) root = path;
    }
    m_listModel->clear();
    m_treeModel->beginReset();
    m_tree.setRoot(root);
    m_treeModel->endReset();
}

void MainWindow::openPath(const QString& pathIn)
{
    QString path = QDir::cleanPath(QFileInfo(pathIn).isRelative() ? QDir::current().absoluteFilePath(pathIn) : pathIn);
    if (path.endsWith(".oiv", Qt::CaseInsensitive) && QFileInfo(path).isFile()) {
        OivDialog dlg(path, m_s, this);
        dlg.exec();
        return;
    }
    setRootForPath(QFileInfo(path).isFile() ? QFileInfo(path).path() : path);
    navigate(path);
    // a file inside an archive (or on disk): open it in its viewer
    VNode* n = m_tree.find(QDir::cleanPath(path));
    if (n && n->path() == QDir::cleanPath(path) && (n->kind == VNode::ArcFile || n->kind == VNode::FsFile)) openNode(n);
}

void MainWindow::navigate(const QString& pathIn, bool record)
{
    QString path = QDir::cleanPath(pathIn);
    VNode* root = m_tree.root();
    if (!root || !(path == root->fsPath || path.startsWith(root->fsPath + "/") || root->fsPath == "/")) setRootForPath(path);
    int gen = m_tree.generation;
    VNode* n = m_tree.find(path);
    if (!n) return;
    VNode* select = nullptr;
    if (!n->isContainer()) { select = n; n = n->parent; }
    if (gen != m_tree.generation) {  // nodes were created outside the model's knowledge
        m_listModel->clear();
        m_treeModel->beginReset();
        m_treeModel->endReset();
    }
    VNode* prev = currentFolder();
    if (record && prev && prev != n) { m_back << prev->path(); m_forward.clear(); }
    m_navigating = true;
    m_listModel->setFolder(n);
    // expand the tree to the node
    std::vector<VNode*> chain;
    for (VNode* c = n; c && c != m_tree.root(); c = c->parent) chain.insert(chain.begin(), c);
    for (VNode* c : chain) {
        QModelIndex idx = m_treeModel->indexFor(c);
        if (m_treeModel->canFetchMore(idx)) m_treeModel->fetchMore(idx);
        if (c != n) m_treeView->expand(idx);
    }
    QModelIndex ti = m_treeModel->indexFor(n);
    m_treeView->setCurrentIndex(ti);
    m_treeView->scrollTo(ti);
    m_navigating = false;
    m_address->setText(n->path());
    if (select) {
        for (int r = 0; r < m_listModel->rowCount({}); ++r)
            if (m_listModel->node(m_listModel->index(r, 0)) == select) {
                m_listView->selectRow(r);
                m_listView->scrollTo(m_listModel->index(r, 0));
            }
    }
    if (!n->error.isEmpty()) statusBar()->showMessage("Error: " + n->error);
    else statusBar()->showMessage(QStringLiteral("%1 items").arg(n->children.size()));
    updateActions();
}

void MainWindow::onTreeSelected()
{
    if (m_navigating) return;
    VNode* n = m_treeModel->node(m_treeView->currentIndex());
    if (n && n != currentFolder()) navigate(n->path());
}

void MainWindow::onActivated(const QModelIndex& idx)
{
    VNode* n = m_listModel->node(idx);
    if (!n) return;
    if (n->isContainer()) navigate(n->path());
    else openNode(n);
}

void MainWindow::goBack()
{
    if (m_back.isEmpty()) return;
    if (currentFolder()) m_forward << currentFolder()->path();
    navigate(m_back.takeLast(), false);
}

void MainWindow::goForward()
{
    if (m_forward.isEmpty()) return;
    if (currentFolder()) m_back << currentFolder()->path();
    navigate(m_forward.takeLast(), false);
}

void MainWindow::goUp()
{
    VNode* f = currentFolder();
    if (f && f->parent) navigate(f->parent->path());
    else if (f) { QString up = QFileInfo(f->fsPath).path(); if (up != f->fsPath) { setRootForPath(up); navigate(up); } }
}

VNode* MainWindow::currentFolder() const { return m_listModel->folder(); }

QList<VNode*> MainWindow::selectedNodes() const
{
    QList<VNode*> out;
    for (const QModelIndex& i : m_listView->selectionModel()->selectedRows())
        if (VNode* n = m_listModel->node(i)) out << n;
    return out;
}

void MainWindow::updateActions()
{
    m_backAct->setEnabled(!m_back.isEmpty());
    m_fwdAct->setEnabled(!m_forward.isEmpty());
    for (QAction* a : m_editOnly) a->setEnabled(m_editMode);
}

// ------------------------------------------------------------------ dialogs

void MainWindow::openFolderDialog()
{
    QString d = QFileDialog::getExistingDirectory(this, "Open folder", currentFolder() ? currentFolder()->fsPath : QDir::homePath());
    if (!d.isEmpty()) openPath(d);
}

void MainWindow::openArchiveDialog()
{
    QString f = QFileDialog::getOpenFileName(this, "Open archive", m_s.gameDir, "RPF archives (*.rpf);;Mod packages (*.oiv);;All files (*)");
    if (!f.isEmpty()) openPath(f);
}

void MainWindow::settings()
{
    QString oldGame = m_s.gameDir;
    SettingsDialog dlg(m_s, this);
    if (dlg.exec() == QDialog::Accepted) {
        m_s.loadKeys();
        updateKeyStatus();
        if (m_s.gameDir != oldGame && !m_s.gameDir.isEmpty()) openPath(m_s.gameDir);
        else if (currentFolder()) {
            VNode* top = currentFolder()->topArchive();
            if (top) reloadUnder(top, currentFolder()->path());
        }
    }
}

void MainWindow::installPackage()
{
    QString f = QFileDialog::getOpenFileName(this, "Install package", QString(), "OpenIV packages (*.oiv);;All files (*)");
    if (f.isEmpty()) return;
    OivDialog dlg(f, m_s, this);
    dlg.exec();
    if (dlg.installed() && currentFolder()) {
        QString p = currentFolder()->path();
        setRootForPath(p);
        navigate(p, false);
    }
}

void MainWindow::toggleEditMode(bool on)
{
    m_editMode = on;
    if (m_editAct->isChecked() != on) m_editAct->setChecked(on);
    m_editLabel->setText(on ? "<b style='color:#c60'>EDIT MODE</b>" : "Read-only");
    updateActions();
}

bool MainWindow::ensureEditMode()
{
    if (m_editMode) return true;
    if (QMessageBox::question(this, "Edit mode", "This change requires Edit mode. Enable it now?") != QMessageBox::Yes) return false;
    toggleEditMode(true);
    return true;
}

// ------------------------------------------------------------------ editing

static bool copyWithProgress(QWidget* parent, const QString& src, const QString& dst, QString* err)
{
    QFile in(src), out(dst + ".part");
    if (!in.open(QIODevice::ReadOnly)) { *err = "Cannot read " + src; return false; }
    QDir().mkpath(QFileInfo(dst).absolutePath());
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) { *err = "Cannot write " + dst; return false; }
    QProgressDialog prog("Copying " + QFileInfo(src).fileName() + " to the mods folder...", "Cancel", 0, 1000, parent);
    prog.setWindowModality(Qt::WindowModal);
    prog.setMinimumDuration(300);
    const qint64 total = std::max<qint64>(1, in.size());
    qint64 done = 0;
    while (!in.atEnd()) {
        QByteArray b = in.read(16 << 20);
        if (out.write(b) != b.size()) { *err = "Write failed: " + out.errorString(); out.remove(); return false; }
        done += b.size();
        prog.setValue(int(done * 1000 / total));
        if (prog.wasCanceled()) { out.remove(); *err = "Cancelled"; return false; }
    }
    out.close();
    QFile::remove(dst);
    if (!out.rename(dst)) { *err = "Cannot rename to " + dst; return false; }
    return true;
}

bool MainWindow::prepareArchiveEdit(VNode*& container)
{
    VNode* top = container->topArchive();
    if (!top || !m_s.useModsFolder || m_s.gameDir.isEmpty()) return true;
    QString game = QDir(m_s.gameDir).absolutePath();
    QString mods = game + "/mods";
    QString disk = top->path();
    if (!disk.startsWith(game + "/") || disk.startsWith(mods + "/")) return true;
    QString rel = disk.mid(game.size() + 1);
    QString modsPath = mods + "/" + rel;
    if (!QFileInfo::exists(modsPath)) {
        QMessageBox box(QMessageBox::Question, "Original game archive",
                        rel + " is an original game archive.\n\nCopy it to the \"mods\" folder and edit the copy? "
                        "(Recommended: the game loads it from mods/ when an ASI loader with a mods-folder plugin is installed.)",
                        QMessageBox::NoButton, this);
        QPushButton* copy = box.addButton("Copy to mods", QMessageBox::AcceptRole);
        QPushButton* orig = box.addButton("Edit original", QMessageBox::DestructiveRole);
        box.addButton(QMessageBox::Cancel);
        box.setDefaultButton(copy);
        box.exec();
        if (box.clickedButton() == orig) return true;
        if (box.clickedButton() != copy) return false;
        QString err;
        if (!copyWithProgress(this, top->fsPath, modsPath, &err)) { QMessageBox::warning(this, "Copy", err); return false; }
        // make the new mods folder visible in the tree
        VNode* root = m_tree.root();
        m_listModel->clear();
        m_treeModel->beginReset();
        m_tree.reset(root);
        m_treeModel->endReset();
    }
    QString newPath = modsPath + container->path().mid(disk.size());
    navigate(newPath);
    VNode* n = m_tree.find(newPath);
    if (!n || n->path() != newPath) { QMessageBox::warning(this, "Mods", "Could not open the mods copy at " + newPath); return false; }
    container = n;
    statusBar()->showMessage("Editing the copy in the mods folder", 5000);
    return true;
}

void MainWindow::reloadUnder(VNode* top, const QString& restorePath)
{
    m_listModel->clear();
    m_treeModel->beginReset();
    m_tree.reset(top);
    m_treeModel->endReset();
    navigate(restorePath, false);
}

bool MainWindow::editContainer(VNode* container, const std::function<bool(rs::RpfNode&, QString*)>& fn, const QString& what)
{
    if (!ensureEditMode()) return false;
    if (!prepareArchiveEdit(container)) return false;
    VNode* archNode = container;
    while (archNode && archNode->kind != VNode::Archive) archNode = archNode->parent;
    if (!archNode) return false;
    m_tree.populate(archNode);
    if (!archNode->arc) { QMessageBox::warning(this, what, "Cannot open archive: " + archNode->error); return false; }
    QString inner = container->kind == VNode::ArcDir ? container->innerPath() : QString();
    QString restore = currentFolder() ? currentFolder()->path() : container->path();
    QProgressDialog prog(what + "...", QString(), 0, 0, this);
    prog.setWindowModality(Qt::WindowModal);
    prog.setMinimumDuration(400);
    QApplication::setOverrideCursor(Qt::WaitCursor);
    QString err;
    bool ok = rs::modifyArchive(archNode->arc, [&](rs::RpfNode& root, QString* e) {
        rs::RpfNode* d = inner.isEmpty() ? &root : root.findPath(inner);
        if (!d || !d->isDir()) { *e = "Folder not found in archive: " + inner; return false; }
        return fn(*d, e);
    }, &err, false, [&](const QString& s) { prog.setLabelText(s); QApplication::processEvents(QEventLoop::ExcludeUserInputEvents); });
    QApplication::restoreOverrideCursor();
    prog.close();
    reloadUnder(archNode->topArchive(), restore);
    if (!ok) { QMessageBox::warning(this, what, what + " failed:\n" + err); return false; }
    statusBar()->showMessage(what + ": done", 5000);
    return true;
}

bool MainWindow::editFsFolder(VNode* folder, const std::function<bool(const QString&, QString*)>& fn)
{
    if (!ensureEditMode()) return false;
    QString err;
    bool ok = fn(folder->fsPath, &err);
    reloadUnder(folder, currentFolder() ? currentFolder()->path() : folder->path());
    if (!ok) QMessageBox::warning(this, "Error", err);
    return ok;
}

static void addDiskPath(rs::RpfNode& dir, const QString& path)
{
    QFileInfo fi(path);
    if (fi.isDir()) {
        rs::RpfNode* d = dir.child(fi.fileName());
        if (!d || !d->isDir()) d = dir.addChild(rs::newDirNode(fi.fileName()));
        for (const QFileInfo& c : QDir(path).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot))
            addDiskPath(*d, c.absoluteFilePath());
    } else {
        dir.addChild(rs::newDiskFileNode(fi.fileName(), fi.absoluteFilePath()));
    }
}

static bool copyRecursive(const QString& src, const QString& dstDir, QString* err)
{
    QFileInfo fi(src);
    QString dst = QDir(dstDir).filePath(fi.fileName());
    if (fi.isDir()) {
        QDir().mkpath(dst);
        for (const QFileInfo& c : QDir(src).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot))
            if (!copyRecursive(c.absoluteFilePath(), dst, err)) return false;
        return true;
    }
    QFile::remove(dst);
    if (!QFile::copy(src, dst)) { *err = "Cannot copy " + src + " to " + dst; return false; }
    return true;
}

void MainWindow::addFilesTo(VNode* container, const QStringList& paths)
{
    if (!container || paths.isEmpty()) return;
    if (container->kind == VNode::FsDir) {
        editFsFolder(container, [&](const QString& dir, QString* err) {
            for (const QString& p : paths)
                if (!copyRecursive(p, dir, err)) return false;
            return true;
        });
        return;
    }
    editContainer(container, [&](rs::RpfNode& dir, QString* err) {
        for (const QString& p : paths) {
            if (!QFileInfo::exists(p)) { *err = "No such file: " + p; return false; }
            addDiskPath(dir, p);
        }
        return true;
    }, QStringLiteral("Importing %1 item(s)").arg(paths.size()));
}

void MainWindow::filesDropped(const QStringList& paths)
{
    QStringList real;
    QString ownPrefix = QDir::tempPath() + "/" + QCoreApplication::applicationName();
    for (const QString& p : paths)
        if (!p.startsWith(ownPrefix)) real << p;
    if (!real.isEmpty()) addFilesTo(currentFolder(), real);
}

void MainWindow::importFiles()
{
    if (!currentFolder()) return;
    QStringList f = QFileDialog::getOpenFileNames(this, "Import files into " + currentFolder()->name);
    addFilesTo(currentFolder(), f);
}

void MainWindow::importFolder()
{
    if (!currentFolder()) return;
    QString d = QFileDialog::getExistingDirectory(this, "Import folder into " + currentFolder()->name);
    if (!d.isEmpty()) addFilesTo(currentFolder(), {d});
}

void MainWindow::exportSelected()
{
    QList<VNode*> sel = selectedNodes();
    if (sel.isEmpty() && currentFolder()) sel << currentFolder();
    if (sel.isEmpty()) return;
    QString err;
    int count = 0;
    if (sel.size() == 1 && !sel[0]->isContainer()) {
        QString p = QFileDialog::getSaveFileName(this, "Export", sel[0]->name);
        if (p.isEmpty()) return;
        QString tmpDir = QFileInfo(p).absolutePath();
        if (!exportNode(sel[0], tmpDir, &err, &count)) { QMessageBox::warning(this, "Export", err); return; }
        if (QFileInfo(p).fileName() != sel[0]->name) {
            QFile::remove(p);
            QFile::rename(QDir(tmpDir).filePath(sel[0]->name), p);
        }
        statusBar()->showMessage("Exported " + p, 5000);
        return;
    }
    QString dir = QFileDialog::getExistingDirectory(this, "Export to folder");
    if (dir.isEmpty()) return;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    for (VNode* n : sel)
        if (!exportNode(n, dir, &err, &count)) { QApplication::restoreOverrideCursor(); QMessageBox::warning(this, "Export", err); return; }
    QApplication::restoreOverrideCursor();
    statusBar()->showMessage(QStringLiteral("Exported %1 file(s) to %2").arg(count).arg(dir), 8000);
}

void MainWindow::replaceSelected()
{
    QList<VNode*> sel = selectedNodes();
    if (sel.size() != 1 || sel[0]->isContainer()) { QMessageBox::information(this, "Replace", "Select one file to replace."); return; }
    VNode* n = sel[0];
    QString src = QFileDialog::getOpenFileName(this, "Replace " + n->name);
    if (src.isEmpty()) return;
    QString name = n->name;
    if (n->kind == VNode::FsFile) {
        editFsFolder(n->parent, [&](const QString& dir, QString* err) {
            QString dst = QDir(dir).filePath(name);
            QFile::remove(dst);
            if (!QFile::copy(src, dst)) { *err = "Cannot copy"; return false; }
            return true;
        });
        return;
    }
    editContainer(n->parent, [&](rs::RpfNode& dir, QString*) {
        dir.addChild(rs::newDiskFileNode(name, src));
        return true;
    }, "Replacing " + name);
}

void MainWindow::deleteSelected()
{
    QList<VNode*> sel = selectedNodes();
    if (sel.isEmpty() || !currentFolder()) return;
    if (!ensureEditMode()) return;
    if (QMessageBox::question(this, "Delete", QStringLiteral("Delete %1 item(s)?").arg(sel.size())) != QMessageBox::Yes) return;
    QStringList names;
    for (auto* n : sel) names << n->name;
    VNode* folder = currentFolder();
    if (folder->kind == VNode::FsDir) {
        editFsFolder(folder, [&](const QString& dir, QString* err) {
            for (const QString& nm : names) {
                QString p = QDir(dir).filePath(nm);
                bool ok = QFileInfo(p).isDir() ? QDir(p).removeRecursively() : QFile::remove(p);
                if (!ok) { *err = "Cannot delete " + p; return false; }
            }
            return true;
        });
        return;
    }
    editContainer(folder, [&](rs::RpfNode& dir, QString* err) {
        for (const QString& nm : names) {
            rs::RpfNode* c = dir.child(nm);
            if (!c) { *err = "Not found: " + nm; return false; }
            dir.take(c);
        }
        return true;
    }, QStringLiteral("Deleting %1 item(s)").arg(names.size()));
}

void MainWindow::renameSelected()
{
    QList<VNode*> sel = selectedNodes();
    if (sel.size() != 1) return;
    VNode* n = sel[0];
    bool ok;
    QString old = n->name;
    QString nn = QInputDialog::getText(this, "Rename", "New name:", QLineEdit::Normal, old, &ok).trimmed();
    if (!ok || nn.isEmpty() || nn == old) return;
    if (nn.contains('/') || nn.contains('\\')) { QMessageBox::warning(this, "Rename", "Names cannot contain slashes."); return; }
    if (n->parent->kind == VNode::FsDir) {
        editFsFolder(n->parent, [&](const QString& dir, QString* err) {
            if (!QFile::rename(QDir(dir).filePath(old), QDir(dir).filePath(nn))) { *err = "Rename failed"; return false; }
            return true;
        });
        return;
    }
    editContainer(n->parent, [&](rs::RpfNode& dir, QString* err) {
        rs::RpfNode* c = dir.child(old);
        if (!c) { *err = "Not found: " + old; return false; }
        if (dir.child(nn) && dir.child(nn) != c) { *err = nn + " already exists"; return false; }
        c->name = nn;
        return true;
    }, "Renaming " + old);
}

void MainWindow::newFolder()
{
    VNode* f = currentFolder();
    if (!f) return;
    bool ok;
    QString name = QInputDialog::getText(this, "New folder", "Folder name:", QLineEdit::Normal, "newfolder", &ok).trimmed();
    if (!ok || name.isEmpty()) return;
    if (f->kind == VNode::FsDir) {
        editFsFolder(f, [&](const QString& dir, QString* err) {
            if (!QDir(dir).mkdir(name)) { *err = "Cannot create folder"; return false; }
            return true;
        });
        return;
    }
    editContainer(f, [&](rs::RpfNode& dir, QString* err) {
        if (dir.child(name)) { *err = name + " already exists"; return false; }
        dir.ensureDir(name);
        return true;
    }, "Creating folder " + name);
}

void MainWindow::newArchive()
{
    VNode* f = currentFolder();
    if (!f) return;
    bool ok;
    QString name = QInputDialog::getText(this, "New archive", "Archive name:", QLineEdit::Normal, "new.rpf", &ok).trimmed();
    if (!ok || name.isEmpty()) return;
    if (!name.endsWith(".rpf", Qt::CaseInsensitive)) name += ".rpf";
    if (f->kind == VNode::FsDir) {
        editFsFolder(f, [&](const QString& dir, QString* err) {
            QString p = QDir(dir).filePath(name);
            if (QFileInfo::exists(p)) { *err = name + " already exists"; return false; }
            return rs::createEmptyArchive(p, err);
        });
        return;
    }
    auto empty = rs::newDirNode();
    QBuffer buf;
    buf.open(QIODevice::ReadWrite);
    QString err;
    if (!rs::writeRpf(*empty, buf, &err)) { QMessageBox::warning(this, "New archive", err); return; }
    QByteArray bytes = buf.data();
    editContainer(f, [&](rs::RpfNode& dir, QString* e) {
        if (dir.child(name)) { *e = name + " already exists"; return false; }
        auto n = rs::newFileNode(name, bytes);
        n->compress = false;
        dir.addChild(std::move(n));
        return true;
    }, "Creating archive " + name);
}

void MainWindow::defragment()
{
    VNode* f = currentFolder();
    VNode* arch = f;
    while (arch && arch->kind != VNode::Archive) arch = arch->parent;
    if (!arch) { QMessageBox::information(this, "Defragment", "Open an archive first."); return; }
    if (!ensureEditMode()) return;
    VNode* c = arch;
    if (!prepareArchiveEdit(c)) return;
    arch = c;
    m_tree.populate(arch);
    if (!arch->arc) return;
    QString restore = currentFolder()->path();
    qint64 before = QFileInfo(arch->topArchive()->fsPath).size();
    QApplication::setOverrideCursor(Qt::WaitCursor);
    QString err;
    bool ok = rs::modifyArchive(arch->arc, nullptr, &err, true);
    QApplication::restoreOverrideCursor();
    QString top = arch->topArchive()->fsPath;
    reloadUnder(arch->topArchive(), restore);
    if (!ok) QMessageBox::warning(this, "Defragment", err);
    else statusBar()->showMessage(QStringLiteral("Rebuilt: %1 -> %2").arg(formatSize(before)).arg(formatSize(QFileInfo(top).size())), 8000);
}

// ------------------------------------------------------------------ viewers

void MainWindow::openNode(VNode* n)
{
    FileHandle h;
    h.name = n->name;
    h.displayPath = n->path();
    QString err;
    if (n->kind == VNode::FsFile) {
        if (n->name.endsWith(".oiv", Qt::CaseInsensitive)) { openPath(n->fsPath); return; }
        QFile f(n->fsPath);
        if (!f.open(QIODevice::ReadOnly)) { QMessageBox::warning(this, "Open", "Cannot read " + n->fsPath); return; }
        h.data = f.readAll();
    } else if (n->kind == VNode::ArcFile) {
        QApplication::setOverrideCursor(Qt::WaitCursor);
        bool ok = n->arc->extract(n->entry, h.data, &err);
        QApplication::restoreOverrideCursor();
        if (!ok) { QMessageBox::warning(this, "Open", err); return; }
    } else {
        return;
    }
    QString path = h.displayPath;
    h.save = [this, path](const QByteArray& d, QString* e) { return saveFileAt(path, d, e); };
    Viewer* v = createViewer(h, nullptr, &err);
    if (!v) {
        QMessageBox::warning(this, "Open", "Cannot open " + n->name + ":\n" + err + "\n\nShowing raw data instead.");
        v = createViewer(FileHandle{h.name + ".bin", h.displayPath, h.data, nullptr}, nullptr, &err);
    }
    auto* w = new ViewerWindow(h, v, this);
    w->show();
}

bool MainWindow::saveFileAt(const QString& path, const QByteArray& data, QString* err)
{
    VNode* n = m_tree.find(path);
    if (!n || n->path() != path) { *err = "The file no longer exists at " + path; return false; }
    QString name = n->name;
    if (n->kind == VNode::FsFile) {
        if (!ensureEditMode()) { *err = "Edit mode is off"; return false; }
        QFile f(n->fsPath);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate) || f.write(data) != data.size()) { *err = "Cannot write " + n->fsPath; return false; }
        return true;
    }
    if (n->kind != VNode::ArcFile) { *err = "Not a file"; return false; }
    bool ok = editContainer(n->parent, [&](rs::RpfNode& dir, QString*) {
        dir.addChild(rs::newFileNode(name, data));
        return true;
    }, "Saving " + name);
    if (!ok) *err = "Not saved";
    return ok;
}

// ------------------------------------------------------------------ misc

void MainWindow::properties()
{
    QList<VNode*> sel = selectedNodes();
    VNode* n = sel.isEmpty() ? currentFolder() : sel[0];
    if (!n) return;
    QStringList lines;
    lines << "Path: " + n->path() << "Type: " + typeNameFor(n);
    if (n->kind == VNode::ArcFile) {
        const rs::RpfEntry& e = n->arc->entry(n->entry);
        lines << QStringLiteral("Size: %1 (%2 bytes)").arg(formatSize(e.fileSize())).arg(e.fileSize())
              << QStringLiteral("Stored size: %1 bytes at offset 0x%2").arg(e.storedSize()).arg(qint64(e.offsetBlocks) * 512, 0, 16)
              << QStringLiteral("Compressed: %1   Encrypted: %2").arg(e.isCompressed() || e.isResource() ? "yes" : "no", e.encrypted ? "yes" : "no");
        if (e.isResource())
            lines << QStringLiteral("Resource version: %1   System flags: 0x%2   Graphics flags: 0x%3")
                         .arg(e.resourceVersion()).arg(e.systemFlags, 8, 16, QChar('0')).arg(e.graphicsFlags, 8, 16, QChar('0'));
        lines << QStringLiteral("Name hash: 0x%1").arg(rs::joaatLower(e.name), 8, 16, QChar('0'));
    } else if (n->kind == VNode::Archive) {
        m_tree.populate(n);
        if (n->arc) {
            int files = 0, dirs = 0;
            for (auto& e : n->arc->entries()) (e.isDir() ? dirs : files)++;
            lines << "Size: " + formatSize(n->arc->size()) << "Encryption: " + rs::encryptionName(n->arc->encryption())
                  << QStringLiteral("Entries: %1 files, %2 folders").arg(files).arg(dirs);
        } else lines << "Error: " + n->error;
    } else if (n->kind == VNode::FsFile) {
        lines << "Size: " + formatSize(n->size);
    }
    QMessageBox::information(this, "Properties - " + n->name, lines.join("\n"));
}

void MainWindow::findFiles()
{
    VNode* start = currentFolder();
    if (!start) return;
    bool ok;
    QString pattern = QInputDialog::getText(this, "Find files", "Search in " + start->name + " (including archives) for name (wildcards allowed):",
                                            QLineEdit::Normal, "*.ytd", &ok).trimmed();
    if (!ok || pattern.isEmpty()) return;
    QRegularExpression rx(QRegularExpression::wildcardToRegularExpression(pattern.contains('*') || pattern.contains('?') ? pattern : "*" + pattern + "*"),
                          QRegularExpression::CaseInsensitiveOption);
    QProgressDialog prog("Searching...", "Stop", 0, 0, this);
    prog.setWindowModality(Qt::WindowModal);
    prog.setMinimumDuration(200);
    QStringList results;
    std::vector<VNode*> stack{start};
    int visited = 0;
    QString startPath = start->path();
    while (!stack.empty() && !prog.wasCanceled() && results.size() < 20000) {
        VNode* n = stack.back();
        stack.pop_back();
        m_tree.populate(n);
        if (++visited % 20 == 0) {
            prog.setLabelText(QStringLiteral("Searching %1\n%2 found").arg(n->path().mid(startPath.size())).arg(results.size()));
            QApplication::processEvents();
        }
        for (auto& c : n->children) {
            if (rx.match(c->name).hasMatch()) results << c->path();
            if (c->isContainer()) stack.push_back(c.get());
        }
    }
    prog.close();
    // nodes were created behind the model's back
    m_listModel->clear();
    m_treeModel->beginReset();
    m_treeModel->endReset();
    navigate(startPath, false);

    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("%1 result(s) for %2").arg(results.size()).arg(pattern));
    dlg.resize(800, 500);
    auto* l = new QVBoxLayout(&dlg);
    auto* list = new QListWidget;
    for (const QString& r : results) list->addItem(r.mid(startPath.size() + 1));
    l->addWidget(list);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Close);
    l->addWidget(bb);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    connect(list, &QListWidget::itemActivated, &dlg, [&](QListWidgetItem* it) {
        navigate(startPath + "/" + it->text());
        dlg.accept();
    });
    dlg.exec();
}

void MainWindow::contextMenu(const QPoint& p)
{
    QList<VNode*> sel = selectedNodes();
    QMenu m(this);
    if (sel.size() == 1) {
        VNode* n = sel[0];
        m.addAction(n->isContainer() ? "Open" : "Open / View", this, [this, n]() {
            if (n->isContainer()) navigate(n->path()); else openNode(n);
        });
        m.addSeparator();
    }
    if (!sel.isEmpty()) m.addAction("Export...", this, &MainWindow::exportSelected);
    m.addAction("Copy path", this, [sel, this]() {
        QStringList ps;
        for (auto* n : sel) ps << n->path();
        if (ps.isEmpty() && currentFolder()) ps << currentFolder()->path();
        QGuiApplication::clipboard()->setText(ps.join('\n'));
    });
    m.addSeparator();
    auto addEdit = [&](const QString& t, void (MainWindow::*f)()) {
        QAction* a = m.addAction(t, this, f);
        if (!m_editMode) a->setEnabled(false);
    };
    addEdit("Import files...", &MainWindow::importFiles);
    if (sel.size() == 1 && !sel[0]->isContainer()) addEdit("Replace...", &MainWindow::replaceSelected);
    if (sel.size() == 1) addEdit("Rename", &MainWindow::renameSelected);
    if (!sel.isEmpty()) addEdit("Delete", &MainWindow::deleteSelected);
    addEdit("New folder...", &MainWindow::newFolder);
    addEdit("New archive...", &MainWindow::newArchive);
    if (!m_editMode) {
        m.addSeparator();
        m.addAction("Enable Edit mode", this, [this]() { toggleEditMode(true); });
    }
    m.addSeparator();
    m.addAction("Properties", this, &MainWindow::properties);
    m.exec(m_listView->viewport()->mapToGlobal(p));
}
