#include "OivDialog.h"

#include <QStyle>
#include <QApplication>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QVBoxLayout>

OivDialog::OivDialog(const QString& path, const AppSettings& s, QWidget* parent) : QDialog(parent), m_s(s)
{
    setWindowTitle("Package installer - " + QFileInfo(path).fileName());
    resize(720, 560);
    auto* lay = new QVBoxLayout(this);
    QString err;
    if (!m_pkg.open(path, &err)) {
        lay->addWidget(new QLabel("Cannot open package:\n" + err));
        auto* bb = new QDialogButtonBox(QDialogButtonBox::Close);
        connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
        lay->addWidget(bb);
        return;
    }
    m_valid = true;
    auto* head = new QHBoxLayout;
    auto* icon = new QLabel;
    if (!m_pkg.icon.isNull()) icon->setPixmap(QPixmap::fromImage(m_pkg.icon.scaled(96, 96, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
    head->addWidget(icon);
    auto* title = new QLabel(QStringLiteral("<h2>%1</h2>Version %2<br>by %3").arg(m_pkg.name.toHtmlEscaped(), m_pkg.versionText.toHtmlEscaped(),
                                                                                    m_pkg.author.toHtmlEscaped()));
    head->addWidget(title, 1);
    lay->addLayout(head);
    auto* desc = new QLabel(m_pkg.description);
    desc->setWordWrap(true);
    desc->setTextFormat(Qt::PlainText);
    lay->addWidget(desc);

    auto* target = new QGroupBox("Install to");
    auto* tl = new QVBoxLayout(target);
    m_toMods = new QRadioButton("\"mods\" folder (recommended, original game files stay untouched)");
    auto* toGame = new QRadioButton("Game folder (modifies original archives)");
    m_toMods->setChecked(s.useModsFolder);
    toGame->setChecked(!s.useModsFolder);
    tl->addWidget(m_toMods);
    tl->addWidget(toGame);
    tl->addWidget(new QLabel("Game: " + (s.gameDir.isEmpty() ? QString("(not set - see Settings)") : s.gameDir)));
    m_dry = new QCheckBox("Dry run (show what would happen, change nothing)");
    tl->addWidget(m_dry);
    lay->addWidget(target);

    m_log = new QPlainTextEdit;
    m_log->setReadOnly(true);
    m_log->setPlainText("This package will:\n  " + m_pkg.describeOperations().join("\n  "));
    lay->addWidget(m_log, 1);

    auto* bb = new QDialogButtonBox;
    m_installBtn = bb->addButton("Install", QDialogButtonBox::AcceptRole);
    bb->addButton(QDialogButtonBox::Close);
    connect(m_installBtn, &QPushButton::clicked, this, &OivDialog::install);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    lay->addWidget(bb);
    if (s.gameDir.isEmpty()) m_installBtn->setEnabled(false);
}

void OivDialog::install()
{
    rs::GameFS fs;
    fs.gameDir = m_s.gameDir;
    fs.useModsFolder = m_toMods->isChecked();
    if (!fs.useModsFolder && !m_dry->isChecked() &&
        QMessageBox::warning(this, "Install", "Install directly into the original game archives? Consider backing them up first.",
                             QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes)
        return;
    rs::OivInstaller inst(fs);
    inst.dryRun = m_dry->isChecked();
    m_log->clear();
    inst.log = [this](const QString& s) {
        m_log->appendPlainText(s);
        QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    };
    m_installBtn->setEnabled(false);
    QApplication::setOverrideCursor(Qt::WaitCursor);
    QString err;
    bool ok = inst.install(m_pkg, &err);
    QApplication::restoreOverrideCursor();
    m_installBtn->setEnabled(true);
    if (!ok) QMessageBox::warning(this, "Install", "Installation failed:\n" + err);
    else if (!inst.dryRun) { m_installed = true; QMessageBox::information(this, "Install", "\"" + m_pkg.name + "\" was installed."); }
}
