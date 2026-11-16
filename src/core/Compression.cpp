#include "Compression.h"
#include <zlib.h>

namespace rs {

bool inflateRaw(const QByteArray& in, QByteArray& out, qint64 expectedSize)
{
    z_stream zs{};
    if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) return false;
    out.clear();
    qint64 cap = expectedSize > 0 ? expectedSize : std::max<qint64>(in.size() * 4, 4096);
    out.resize(int(cap));
    zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(in.constData()));
    zs.avail_in = uInt(in.size());
    qint64 produced = 0;
    int ret = Z_OK;
    while (true) {
        if (produced == out.size()) out.resize(int(std::min<qint64>(qint64(out.size()) * 2, qint64(INT32_MAX - 64))));
        zs.next_out = reinterpret_cast<Bytef*>(out.data() + produced);
        zs.avail_out = uInt(out.size() - produced);
        ret = inflate(&zs, Z_NO_FLUSH);
        produced = qint64(zs.total_out);
        if (ret == Z_STREAM_END) break;
        if (ret != Z_OK && ret != Z_BUF_ERROR) break;
        if (ret == Z_BUF_ERROR && zs.avail_in == 0) break; // truncated input
    }
    inflateEnd(&zs);
    out.resize(int(produced));
    return ret == Z_STREAM_END || (ret == Z_BUF_ERROR && produced > 0) || (ret == Z_OK && produced > 0);
}

QByteArray deflateRaw(const QByteArray& in, int level)
{
    z_stream zs{};
    if (deflateInit2(&zs, level, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK) return {};
    QByteArray out;
    out.resize(int(deflateBound(&zs, uLong(in.size()))));
    zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(in.constData()));
    zs.avail_in = uInt(in.size());
    zs.next_out = reinterpret_cast<Bytef*>(out.data());
    zs.avail_out = uInt(out.size());
    deflate(&zs, Z_FINISH);
    out.resize(int(zs.total_out));
    deflateEnd(&zs);
    return out;
}

} // namespace rs
