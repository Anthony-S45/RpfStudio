#pragma once
#include "Viewers.h"

class QPlainTextEdit;
class QLineEdit;

class TextViewer : public Viewer {
    Q_OBJECT
public:
    explicit TextViewer(QWidget* parent = nullptr);
    void load(const QByteArray& d, const QString& ext);
    bool loadRbf(const QByteArray& d, QString* err);
    bool isEditable() const override { return !m_readOnly; }
    QByteArray serialize(QString* err) override;
    void addActions(QToolBar* tb) override;
    void markSaved() override;
private:
    void find(bool backwards);
    QPlainTextEdit* m_edit;
    QLineEdit* m_find;
    bool m_readOnly = false, m_crlf = false, m_bom = false;
};
