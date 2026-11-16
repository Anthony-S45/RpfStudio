// Little-endian binary helpers shared by all format code.
#pragma once
#include <QByteArray>
#include <QString>
#include <cstdint>
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace rs {

class FormatError : public std::runtime_error {
public:
    explicit FormatError(const QString& msg) : std::runtime_error(msg.toStdString()) {}
    QString qmessage() const { return QString::fromStdString(what()); }
};

inline uint16_t rd16(const void* p) { uint16_t v; std::memcpy(&v, p, 2); return v; }
inline uint32_t rd32(const void* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
inline uint64_t rd64(const void* p) { uint64_t v; std::memcpy(&v, p, 8); return v; }
inline float rdf(const void* p) { float v; std::memcpy(&v, p, 4); return v; }
inline void wr16(void* p, uint16_t v) { std::memcpy(p, &v, 2); }
inline void wr32(void* p, uint32_t v) { std::memcpy(p, &v, 4); }
inline void wr64(void* p, uint64_t v) { std::memcpy(p, &v, 8); }

// Bounds-checked reader over a QByteArray.
class Reader {
public:
    explicit Reader(const QByteArray& d, qint64 pos = 0) : m_d(d), m_pos(pos) {}
    qint64 pos() const { return m_pos; }
    void seek(qint64 p) { m_pos = p; }
    qint64 size() const { return m_d.size(); }
    bool atEnd() const { return m_pos >= m_d.size(); }
    void need(qint64 n) const {
        if (n < 0 || m_pos < 0 || m_pos + n > m_d.size())
            throw FormatError(QStringLiteral("Unexpected end of data at offset %1").arg(m_pos));
    }
    uint8_t u8() { need(1); return uint8_t(m_d[int(m_pos++)]); }
    uint16_t u16() { need(2); auto v = rd16(m_d.constData() + m_pos); m_pos += 2; return v; }
    uint32_t u32() { need(4); auto v = rd32(m_d.constData() + m_pos); m_pos += 4; return v; }
    uint64_t u64() { need(8); auto v = rd64(m_d.constData() + m_pos); m_pos += 8; return v; }
    int16_t i16() { return int16_t(u16()); }
    int32_t i32() { return int32_t(u32()); }
    float f32() { need(4); auto v = rdf(m_d.constData() + m_pos); m_pos += 4; return v; }
    QByteArray bytes(qint64 n) { need(n); auto b = m_d.mid(int(m_pos), int(n)); m_pos += n; return b; }
    QByteArray cstr() {
        qint64 e = m_pos;
        while (e < m_d.size() && m_d[int(e)] != 0) ++e;
        auto b = m_d.mid(int(m_pos), int(e - m_pos));
        m_pos = std::min<qint64>(e + 1, m_d.size());
        return b;
    }
private:
    const QByteArray& m_d;
    qint64 m_pos;
};

// Growable little-endian writer.
class Writer {
public:
    QByteArray data;
    void u8(uint8_t v) { data.append(char(v)); }
    void u16(uint16_t v) { char b[2]; wr16(b, v); data.append(b, 2); }
    void u32(uint32_t v) { char b[4]; wr32(b, v); data.append(b, 4); }
    void u64(uint64_t v) { char b[8]; wr64(b, v); data.append(b, 8); }
    void f32(float v) { char b[4]; std::memcpy(b, &v, 4); data.append(b, 4); }
    void bytes(const QByteArray& b) { data.append(b); }
    void cstr(const QByteArray& s) { data.append(s); data.append('\0'); }
    void pad(int align) { while (data.size() % align) data.append('\0'); }
    qint64 pos() const { return data.size(); }
};

inline qint64 alignUp(qint64 v, qint64 a) { return (v + a - 1) / a * a; }

} // namespace rs
