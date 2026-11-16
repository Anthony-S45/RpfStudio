#pragma once
#include <QDialog>
#include <QString>

class QLineEdit;
class QCheckBox;
class QLabel;
class QPushButton;

struct AppSettings {
    QString gameDir;
    QString keyDir;
    bool useModsFolder = true;
    bool confirmModsCopy = true;

    static AppSettings load();
    void save() const;
    static QString defaultKeyDir();
    // Loads keys from keyDir into the global key store. Returns a status message.
    QString loadKeys() const;
};

class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit SettingsDialog(AppSettings& s, QWidget* parent = nullptr);
private slots:
    void browseGame();
    void detectGame();
    void browseKeys();
    void findKeys();
    void accept() override;
private:
    void updateKeyStatus();
    AppSettings& m_s;
    QLineEdit* m_game;
    QLineEdit* m_keys;
    QCheckBox* m_mods;
    QLabel* m_keyStatus;
};
