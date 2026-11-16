// YTD texture dictionaries (legacy PC layout, resource version 13).
#pragma once
#include "Resource.h"
#include "Texture.h"
#include <vector>

namespace rs {

struct YtdTexture {
    QString name;
    TextureImage image;
    QByteArray rawStruct;   // original 0x90-byte texture struct (keeps unknown fields)
};

class TextureDictionary {
public:
    std::vector<YtdTexture> textures;
    QByteArray rawDict;     // original 0x40-byte dictionary struct
    uint32_t version = 13;

    // Parses a .ytd file (RSC7 bytes).
    bool load(const QByteArray& rsc7, QString* err);
    // Parses a dictionary located at `ptr` inside any resource (e.g. embedded in a drawable).
    bool parseAt(const ResourceData& res, uint64_t ptr, QString* err);
    // Serialises as a .ytd RSC7 file.
    QByteArray build(QString* err) const;

    int find(const QString& name) const;
    void setTexture(const QString& name, const TextureImage& img);  // add or replace
    bool remove(const QString& name);
};

} // namespace rs
