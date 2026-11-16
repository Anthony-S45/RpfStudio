// Texture pixel formats, decoding, DDS import/export and basic BC encoding.
#pragma once
#include <QByteArray>
#include <QImage>
#include <QString>
#include <cstdint>

namespace rs {

enum TexFormat : uint32_t {
    TexA8R8G8B8 = 21,
    TexX8R8G8B8 = 22,
    TexA1R5G5B5 = 25,
    TexA8 = 28,
    TexA8B8G8R8 = 32,
    TexL8 = 50,
    TexDXT1 = 0x31545844,
    TexDXT3 = 0x33545844,
    TexDXT5 = 0x35545844,
    TexATI1 = 0x31495441,
    TexATI2 = 0x32495441,
    TexBC7 = 0x20374342,
};

QString texFormatName(uint32_t fmt);
bool texFormatKnown(uint32_t fmt);
bool texIsBlock(uint32_t fmt);
int texBlockBytes(uint32_t fmt);     // block formats: bytes per 4x4 block
int texBitsPerPixel(uint32_t fmt);
qint64 texRowPitch(uint32_t fmt, int width);
qint64 texLevelSize(uint32_t fmt, int width, int height);
qint64 texChainSize(uint32_t fmt, int width, int height, int levels);

struct TextureImage {
    uint32_t format = TexDXT5;
    int width = 0, height = 0, levels = 1;
    QByteArray data;   // all mip levels, largest first
};

// Decodes one level to an RGBA8888 image (null image if unsupported/short).
QImage decodeTextureLevel(uint32_t fmt, int width, int height, const char* data, qint64 size);
QImage decodeTexture(const TextureImage& t, int level = 0);

QByteArray writeDds(const TextureImage& t);
bool readDds(const QByteArray& dds, TextureImage& out, QString* err);

// Encode an image (any size; non-multiple-of-4 sizes are fine) to the given
// format: DXT1, DXT5, A8R8G8B8 or A8B8G8R8. Optionally builds a full mip chain.
TextureImage encodeImage(const QImage& img, uint32_t fmt, bool mipmaps);

} // namespace rs
