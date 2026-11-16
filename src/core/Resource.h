// RSC7 resource container (ydr, ytd, yft, ydd, ybn, ...).
#pragma once
#include <QByteArray>
#include <QString>
#include <cstdint>

namespace rs {

uint32_t resourceSizeFromFlags(uint32_t flags);
// Page flags for a segment of `len` bytes stored as one page (max 256 MB).
uint32_t resourceFlagsSinglePage(qint64 len, uint32_t versionNibble, qint64* pageSize);

struct ResourceData {
    uint32_t version = 0;
    uint32_t systemFlags = 0, graphicsFlags = 0;
    QByteArray system, graphics;

    // Parses an RSC7 file (header + deflated body).
    bool load(const QByteArray& rsc7, QString* err);
    // Serialises system/graphics segments as an RSC7 file using a single page per segment
    // (empty result if a segment exceeds 256 MB).
    QByteArray build() const;

    // Pointer resolution: 0x5xxxxxxx -> system, 0x6xxxxxxx -> graphics.
    bool valid(uint64_t ptr, qint64 len = 1) const;
    const char* at(uint64_t ptr, qint64 len = 1) const;  // nullptr if out of range
    uint8_t u8(uint64_t p) const { auto d = at(p, 1); return d ? uint8_t(*d) : 0; }
    uint16_t u16(uint64_t p) const;
    uint32_t u32(uint64_t p) const;
    uint64_t u64(uint64_t p) const;
    float f32(uint64_t p) const;
    QByteArray bytes(uint64_t p, qint64 len) const;
    QString cstr(uint64_t p) const;
};

// Human readable resource type from extension.
QString resourceTypeName(const QString& ext);

} // namespace rs
