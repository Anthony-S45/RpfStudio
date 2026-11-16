#include "SettingsDialog.h"
#include "Crypto.h"
#include "GameFS.h"

#include <QStyle>
#include <QApplication>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressDialog>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QVBoxLayout>
#include <atomic>
#include <mutex>
#include <thread>

QString AppSettings::defaultKeyDir()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)).filePath("RpfStudio/keys");
}

AppSettings AppSettings::load()
{
    QSettings q("RpfStudio", "RpfStudio");
    AppSettings s;
    s.gameDir = q.value("gameDir").toString();
    s.keyDir = q.value("keyDir", defaultKeyDir()).toString();
    s.useModsFolder = q.value("useModsFolder", true).toBool();
    s.confirmModsCopy = q.value("confirmModsCopy", true).toBool();
    if (s.gameDir.isEmpty()) {
        QStringList g = rs::GameFS::guessGameDirs();
        if (!g.isEmpty()) s.gameDir = g.first();
    }
    return s;
}

void AppSettings::save() const
{
    QSettings q("RpfStudio", "RpfStudio");
    q.setValue("gameDir", gameDir);
    q.setValue("keyDir", keyDir);
    q.setValue("useModsFolder", useModsFolder);
    q.setValue("confirmModsCopy", confirmModsCopy);
}

QString AppSettings::loadKeys() const
{
    rs::GameKeys& k = rs::GameKeys::global();
    k = rs::GameKeys();
    QString err;
    if (QDir(keyDir).exists()) k.loadFromDir(keyDir, &err);
    return k.summary();
}

SettingsDialog::SettingsDialog(AppSettings& s, QWidget* parent) : QDialog(parent), m_s(s)
{
    setWindowTitle("Settings");
    resize(640, 360);
    auto* lay = new QVBoxLayout(this);

    auto* gameBox = new QGroupBox("Game");
    auto* gf = new QFormLayout(gameBox);
    auto* gl = new QHBoxLayout;
    m_game = new QLineEdit(s.gameDir);
    auto* gb = new QPushButton("Browse...");
    auto* gd = new QPushButton("Detect");
    gl->addWidget(m_game);
    gl->addWidget(gb);
    gl->addWidget(gd);
    gf->addRow("GTA V folder:", gl);
    m_mods = new QCheckBox("Edit archives in the \"mods\" folder (copies originals there first, like OpenIV)");
    m_mods->setChecked(s.useModsFolder);
    gf->addRow(m_mods);
    lay->addWidget(gameBox);

    auto* keyBox = new QGroupBox("Encryption keys");
    auto* kf = new QVBoxLayout(keyBox);
    auto* info = new QLabel("GTA V archives are encrypted. Keys are not included with this program; they are read from "
                            "your own copy of the game (GTA5.exe). You can also place gtav_aes_key.dat, gtav_ng_key.dat, "
                            "gtav_ng_decrypt_tables.dat and gtav_hash_lut.dat in the key folder.");
    info->setWordWrap(true);
    kf->addWidget(info);
    auto* kl = new QHBoxLayout;
    m_keys = new QLineEdit(s.keyDir);
    auto* kb = new QPushButton("Browse...");
    kl->addWidget(new QLabel("Key folder:"));
    kl->addWidget(m_keys);
    kl->addWidget(kb);
    kf->addLayout(kl);
    auto* fl = new QHBoxLayout;
    auto* find = new QPushButton("Find keys in GTA5.exe...");
    m_keyStatus = new QLabel;
    m_keyStatus->setWordWrap(true);
    fl->addWidget(find);
    fl->addWidget(m_keyStatus, 1);
    kf->addLayout(fl);
    lay->addWidget(keyBox);
    lay->addStretch();

    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    lay->addWidget(bb);
    connect(bb, &QDialogButtonBox::accepted, this, &SettingsDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(gb, &QPushButton::clicked, this, &SettingsDialog::browseGame);
    connect(gd, &QPushButton::clicked, this, &SettingsDialog::detectGame);
    connect(kb, &QPushButton::clicked, this, &SettingsDialog::browseKeys);
    connect(find, &QPushButton::clicked, this, &SettingsDialog::findKeys);
    updateKeyStatus();
}

void SettingsDialog::updateKeyStatus() { m_keyStatus->setText(rs::GameKeys::global().summary()); }

void SettingsDialog::browseGame()
{
    QString d = QFileDialog::getExistingDirectory(this, "GTA V folder", m_game->text());
    if (!d.isEmpty()) m_game->setText(d);
}

void SettingsDialog::detectGame()
{
    QStringList g = rs::GameFS::guessGameDirs();
    if (g.isEmpty()) {
        QMessageBox::information(this, "Detect", "No GTA V installation found in the usual Steam / Wine / Heroic locations.");
        return;
    }
    m_game->setText(g.first());
}

void SettingsDialog::browseKeys()
{
    QString d = QFileDialog::getExistingDirectory(this, "Key folder", m_keys->text());
    if (!d.isEmpty()) {
        m_keys->setText(d);
        QString err;
        rs::GameKeys::global().loadFromDir(d, &err);
        updateKeyStatus();
    }
}

void SettingsDialog::findKeys()
{
    QString start = QDir(m_game->text()).exists("GTA5.exe") ? QDir(m_game->text()).filePath("GTA5.exe") : m_game->text();
    QString exe = QFileDialog::getOpenFileName(this, "Select GTA5.exe", start, "Executables (*.exe);;All files (*)");
    if (exe.isEmpty()) return;
    QProgressDialog prog("Searching for keys...", QString(), 0, 0, this);
    prog.setWindowModality(Qt::WindowModal);
    prog.setMinimumDuration(0);
    prog.show();
    rs::GameKeys found = rs::GameKeys::global();
    QString err, status;
    std::atomic<bool> done{false};
    std::mutex m;
    bool ok = false;
    std::thread t([&]() {
        ok = found.searchExecutable(exe, [&](const QString& s) { std::lock_guard<std::mutex> l(m); status = s; }, &err);
        done = true;
    });
    while (!done) {
        {
            std::lock_guard<std::mutex> l(m);
            prog.setLabelText(status);
        }
        QApplication::processEvents(QEventLoop::AllEvents, 100);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    t.join();
    prog.close();
    if (!ok) { QMessageBox::warning(this, "Keys", err); return; }
    if (!found.hasNg() && found.hasAes()) {
        auto r = QMessageBox::question(this, "Keys",
            "The AES key was found, but the NG keys are not stored in plain form in this version of GTA5.exe.\n\n"
            "If you have CodeWalker's magic.dat file (it ships with CodeWalker), select it now to unlock the NG keys "
            "with your AES key.\n\nSelect magic.dat?");
        if (r == QMessageBox::Yes) {
            QString magic = QFileDialog::getOpenFileName(this, "Select magic.dat", QString(), "magic.dat (magic.dat);;All files (*)");
            if (!magic.isEmpty() && !found.loadMagicDat(magic, &err)) QMessageBox::warning(this, "Keys", err);
        }
    }
    if (!found.hasAes() && !found.hasNg()) {
        QMessageBox::warning(this, "Keys", "No keys were found in " + exe);
        return;
    }
    rs::GameKeys::global() = found;
    if (!found.saveToDir(m_keys->text(), &err)) QMessageBox::warning(this, "Keys", err);
    updateKeyStatus();
}

void SettingsDialog::accept()
{
    m_s.gameDir = m_game->text();
    m_s.keyDir = m_keys->text();
    m_s.useModsFolder = m_mods->isChecked();
    m_s.save();
    QDialog::accept();
}
