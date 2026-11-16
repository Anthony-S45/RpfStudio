#pragma once
#include "Oiv.h"
#include "SettingsDialog.h"
#include <QDialog>

class QLabel;
class QPlainTextEdit;
class QRadioButton;
class QPushButton;
class QCheckBox;

class OivDialog : public QDialog {
    Q_OBJECT
public:
    OivDialog(const QString& path, const AppSettings& s, QWidget* parent = nullptr);
    bool isValid() const { return m_valid; }
    bool installed() const { return m_installed; }
private slots:
    void install();
private:
    rs::OivPackage m_pkg;
    const AppSettings& m_s;
    bool m_valid = false, m_installed = false;
    QPlainTextEdit* m_log;
    QRadioButton* m_toMods;
    QCheckBox* m_dry;
    QPushButton* m_installBtn;
};
