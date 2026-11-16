// YTD texture dictionaries: legacy PC layout (resource version 13, read/write)
// and GTA V Enhanced "gen9" layout (version 5, read-only).
#pragma once
#include "Resource.h"
#include "Texture.h"
#include <vector>

namespace rs {

struct YtdTexture {
    uint8_t gen9Format = 0; // original DXGI-style format byte (gen9 only)
    QString name;
    TextureImage image;
    QByteArray rawStruct;   // original 0x90-byte texture struct (keeps unknown fields)
};

class TextureDictionary {
public:
    std::vector<YtdTexture> textures;
    QByteArray rawDict;     // original 0x40-byte dictionary struct
    uint32_t version = 13;
    bool gen9 = false;      // Enhanced layout (cannot be rebuilt yet)

    // Parses a .ytd file (RSC7 bytes).
    bool load(const QByteArray& rsc7, QString* err);
    // Parses a dictionary located at `ptr` inside any resource (e.g. embedded in a drawable).
    bool parseAt(const ResourceData& res, uint64_t ptr, QString* err, bool gen9 = false);
    // Serialises as a .ytd RSC7 file.
    QByteArray build(QString* err) const;

    int find(const QString& name) const;
    void setTexture(const QString& name, const TextureImage& img);  // add or replace
    bool remove(const QString& name);
};

} // namespace rs
