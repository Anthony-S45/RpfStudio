#include "Viewers.h"
#include "Gxt2Viewer.h"
#include "HexView.h"
#include "ModelViewer.h"
#include "TextViewer.h"
#include "TextureViewer.h"

#include "Gxt2.h"
#include "Rpf.h"

#include <QStyle>
#include <QCloseEvent>
#include <QFile>
#include <QFileDialog>
#include <QMessageBox>
#include <QStatusBar>
#include <QToolBar>

QString FileHandle::extension() const
{
    int dot = name.lastIndexOf('.');
    return dot < 0 ? QString() : name.mid(dot + 1).toLower();
}

bool looksLikeText(const QByteArray& d)
{
    if (d.isEmpty()) return true;
    int n = std::min<int>(d.size(), 4096), bad = 0;
    int start = d.startsWith("\xEF\xBB\xBF") ? 3 : 0;
    if (d.startsWith("\xFF\xFE") || d.startsWith("\xFE\xFF")) return true;
    for (int i = start; i < n; ++i) {
        uchar c = uchar(d[i]);
        if (c == 0) return false;
        if (c < 9 || (c > 13 && c < 32)) ++bad;
    }
    return bad * 50 < n;
}

Viewer* createViewer(const FileHandle& h, QWidget* parent, QString* err)
{
    const QString ext = h.extension();
    const bool isRsc = h.data.size() >= 16 && *reinterpret_cast<const quint32*>(h.data.constData()) == rs::Rsc7Magic;
    if (ext == "ytd" && isRsc) {
        auto* v = new TextureViewer(parent);
        if (v->load(h.data, err)) return v;
        delete v;
        return nullptr;
    }
    if ((ext == "ydr" || ext == "ydd" || ext == "yft") && isRsc) {
        auto* v = new ModelViewer(parent);
        if (v->load(h.data, ext, err)) return v;
        delete v;
        return nullptr;
    }
    if (ext == "gxt2" || h.data.startsWith("2TXG")) {
        auto* v = new Gxt2Viewer(parent);
        if (v->load(h.data, err)) return v;
        delete v;
        return nullptr;
    }
    if (h.data.startsWith("RBF0")) {
        auto* v = new TextViewer(parent);
        if (v->loadRbf(h.data, err)) return v;
        delete v;
        return nullptr;
    }
    static const QStringList textExts = {"xml", "meta", "txt", "dat", "ide", "ipl", "cfg", "ini", "log", "lua", "json",
                                         "csv", "gfx.txt", "sps", "fxc.txt", "nametable", "ymt.xml", "pso.xml"};
    if (!isRsc && (textExts.contains(ext) || looksLikeText(h.data))) {
        auto* v = new TextViewer(parent);
        v->load(h.data, ext);
        return v;
    }
    return new HexViewer(h.data, parent);
}

ViewerWindow::ViewerWindow(const FileHandle& h, Viewer* v, QWidget* parent) : QMainWindow(parent), m_h(h), m_v(v)
{
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowFlag(Qt::Window);
    setCentralWidget(v);
    v->setParent(this);
    auto* tb = addToolBar("File");
    tb->setMovable(false);
    tb->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    if (v->isEditable() && h.save) {
        m_saveAct = tb->addAction(style()->standardIcon(QStyle::SP_DialogSaveButton), "Save", this, &ViewerWindow::save);
        m_saveAct->setShortcut(QKeySequence::Save);
        m_saveAct->setEnabled(false);
        connect(v, &Viewer::modifiedChanged, this, [this](bool m) { m_saveAct->setEnabled(m); updateTitle(); });
    }
    tb->addAction(style()->standardIcon(QStyle::SP_DriveFDIcon), "Export file...", this, &ViewerWindow::exportRaw);
    tb->addSeparator();
    v->addActions(tb);
    if (v->isEditable() && !h.save) statusBar()->showMessage("Read-only: enable Edit mode in the main window to save changes.");
    resize(1000, 700);
    updateTitle();
}

void ViewerWindow::updateTitle()
{
    setWindowTitle((m_v->isModified() ? "* " : "") + m_h.name + " - " + m_h.displayPath);
}

bool ViewerWindow::save()
{
    QString err;
    QByteArray d = m_v->serialize(&err);
    if (d.isEmpty() && !err.isEmpty()) { QMessageBox::warning(this, "Save", err); return false; }
    if (!m_h.save(d, &err)) { QMessageBox::warning(this, "Save", "Saving failed: " + err); return false; }
    m_h.data = d;
    m_v->markSaved();
    statusBar()->showMessage("Saved " + m_h.displayPath, 5000);
    return true;
}

void ViewerWindow::exportRaw()
{
    QString p = QFileDialog::getSaveFileName(this, "Export file", m_h.name);
    if (p.isEmpty()) return;
    QByteArray d = m_h.data;
    if (m_v->isModified()) {
        QString err;
        QByteArray cur = m_v->serialize(&err);
        if (!cur.isEmpty()) d = cur;
    }
    QFile f(p);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate) || f.write(d) != d.size())
        QMessageBox::warning(this, "Export", "Cannot write " + p);
}

void ViewerWindow::closeEvent(QCloseEvent* e)
{
    if (m_v->isModified()) {
        auto r = QMessageBox::question(this, "Unsaved changes", "Save changes to " + m_h.name + "?",
                                       m_h.save ? (QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel)
                                                : (QMessageBox::Discard | QMessageBox::Cancel));
        if (r == QMessageBox::Cancel || (r == QMessageBox::Save && !save())) { e->ignore(); return; }
    }
    e->accept();
}
