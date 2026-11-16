// Viewer framework: each file type gets a widget, hosted in its own window.
#pragma once
#include <QByteArray>
#include <QMainWindow>
#include <QWidget>
#include <functional>

class QToolBar;

struct FileHandle {
    QString name;          // file name (with extension)
    QString displayPath;   // full virtual path
    QByteArray data;       // game-file bytes
    // Writes new bytes back to where the file came from (null = read only).
    std::function<bool(const QByteArray&, QString*)> save;
    QString extension() const;
};

class Viewer : public QWidget {
    Q_OBJECT
public:
    using QWidget::QWidget;
    virtual bool isEditable() const { return false; }
    // Serialises the edited state; return empty + err on failure.
    virtual QByteArray serialize(QString* err) { if (err) *err = "not editable"; return {}; }
    virtual void addActions(QToolBar*) {}
    // Called after a successful save.
    virtual void markSaved() { setModified(false); }
    bool isModified() const { return m_modified; }
    void setModified(bool m) { if (m != m_modified) { m_modified = m; emit modifiedChanged(m); } }
signals:
    void modifiedChanged(bool);
private:
    bool m_modified = false;
};

class ViewerWindow : public QMainWindow {
    Q_OBJECT
public:
    ViewerWindow(const FileHandle& h, Viewer* v, QWidget* parent = nullptr);
protected:
    void closeEvent(QCloseEvent* e) override;
private slots:
    bool save();
    void exportRaw();
private:
    void updateTitle();
    FileHandle m_h;
    Viewer* m_v;
    QAction* m_saveAct = nullptr;
};

// Chooses a viewer for the file (texture, model, text, string table or hex).
Viewer* createViewer(const FileHandle& h, QWidget* parent, QString* err);
bool looksLikeText(const QByteArray& d);
