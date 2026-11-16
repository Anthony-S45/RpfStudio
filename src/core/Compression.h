#pragma once
#include <QByteArray>

namespace rs {
// Raw DEFLATE (no zlib header), as used by RPF7 and RSC7.
// expectedSize < 0 means unknown.
bool inflateRaw(const QByteArray& in, QByteArray& out, qint64 expectedSize);
QByteArray deflateRaw(const QByteArray& in, int level = 9);
}
