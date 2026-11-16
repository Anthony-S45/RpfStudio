// GXT2 text tables (game strings) and RBF binary XML.
#pragma once
#include <QByteArray>
#include <QString>
#include <cstdint>
#include <vector>

namespace rs {

struct Gxt2Entry {
    uint32_t hash = 0;
    QString text;
};

class Gxt2 {
public:
    std::vector<Gxt2Entry> entries;
    bool load(const QByteArray& data, QString* err);
    QByteArray build() const;
    // Exports/imports the "0xHASH = text" format used by many GTA tools.
    QString toText() const;
    bool fromText(const QString& text, QString* err);
};

// RBF ("RBF0") binary XML to XML text. Returns false if not RBF.
bool rbfToXml(const QByteArray& data, QString& xml, QString* err);

} // namespace rs
