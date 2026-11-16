// OpenIV-compatible .oiv package reading and installation (package format 2.x).
#pragma once
#include "GameFS.h"
#include "Zip.h"
#include <QDomDocument>
#include <QImage>
#include <functional>

namespace rs {

struct OivPackage {
    QString name, versionText, author, description, licence, formatVersion, target;
    QImage icon;
    ZipReader zip;
    QDomDocument assembly;

    bool open(const QString& path, QString* err);
    QDomElement content() const;
    // Human-readable list of operations, for confirmation dialogs.
    QStringList describeOperations() const;
};

class OivInstaller {
public:
    explicit OivInstaller(const GameFS& fs) : m_fs(fs) {}
    std::function<void(const QString&)> log;
    bool dryRun = false;  // resolve and report only; nothing is written

    bool install(const OivPackage& pkg, QString* err);
private:
    const GameFS& m_fs;
};

// Text-file edit helpers (shared with the GUI and tests).
QString applyTextCommands(const QString& original, const QDomElement& textElement, QStringList* notes);
bool applyXmlCommands(QByteArray& xml, const QDomElement& xmlElement, QString* err, QStringList* notes);

} // namespace rs
