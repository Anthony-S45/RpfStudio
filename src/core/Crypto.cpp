#include "Crypto.h"
#include "Compression.h"
#include "KeyHashes.inc"

#include <QDir>
#include <QFile>
#include <openssl/evp.h>
#include <openssl/sha.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace rs {

GameKeys& GameKeys::global()
{
    static GameKeys k;
    return k;
}

QString GameKeys::summary() const
{
    return QStringLiteral("AES key: %1, NG keys: %2, NG tables: %3, hash LUT: %4")
        .arg(hasAes() ? "yes" : "no", hasNg() ? "yes" : (ngKeys.size() == NgKeyCount ? "keys only" : "no"),
             ngTables.size() == size_t(NgTableCount) * 256 ? "yes" : "no", hasLut() ? "yes" : "no");
}

static QByteArray readAll(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return f.readAll();
}

static void setNgKeys(GameKeys& k, const QByteArray& b)
{
    k.ngKeys.assign(GameKeys::NgKeyCount, {});
    for (int i = 0; i < GameKeys::NgKeyCount; ++i)
        std::memcpy(k.ngKeys[i].data(), b.constData() + i * GameKeys::NgKeySize, GameKeys::NgKeySize);
}

static void setNgTables(GameKeys& k, const QByteArray& b)
{
    k.ngTables.resize(size_t(GameKeys::NgTableCount) * 256);
    std::memcpy(k.ngTables.data(), b.constData(), GameKeys::NgTablesSize);
}

bool GameKeys::loadFromDir(const QString& dirPath, QString* err)
{
    QDir dir(dirPath);
    QByteArray aes = readAll(dir.filePath("gtav_aes_key.dat"));
    QByteArray ng = readAll(dir.filePath("gtav_ng_key.dat"));
    QByteArray tab = readAll(dir.filePath("gtav_ng_decrypt_tables.dat"));
    QByteArray lut = readAll(dir.filePath("gtav_hash_lut.dat"));
    int found = 0;
    if (aes.size() == 32) { aesKey = aes; ++found; }
    if (ng.size() == NgKeyCount * NgKeySize) { setNgKeys(*this, ng); ++found; }
    if (tab.size() == NgTablesSize) { setNgTables(*this, tab); ++found; }
    if (lut.size() == 256) { hashLut = lut; ++found; }
    if (!found) {
        if (err) *err = QStringLiteral("No key files found in %1").arg(dirPath);
        return false;
    }
    return true;
}

static bool writeFile(const QString& path, const QByteArray& data)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    return f.write(data) == data.size();
}

bool GameKeys::saveToDir(const QString& dirPath, QString* err) const
{
    QDir().mkpath(dirPath);
    QDir dir(dirPath);
    bool ok = true;
    if (hasAes()) ok &= writeFile(dir.filePath("gtav_aes_key.dat"), aesKey);
    if (ngKeys.size() == NgKeyCount) {
        QByteArray b(NgKeyCount * NgKeySize, 0);
        for (int i = 0; i < NgKeyCount; ++i) std::memcpy(b.data() + i * NgKeySize, ngKeys[i].data(), NgKeySize);
        ok &= writeFile(dir.filePath("gtav_ng_key.dat"), b);
    }
    if (ngTables.size() == size_t(NgTableCount) * 256)
        ok &= writeFile(dir.filePath("gtav_ng_decrypt_tables.dat"),
                        QByteArray(reinterpret_cast<const char*>(ngTables.data()), NgTablesSize));
    if (hasLut()) ok &= writeFile(dir.filePath("gtav_hash_lut.dat"), hashLut);
    if (!ok && err) *err = QStringLiteral("Failed writing key files to %1").arg(dirPath);
    return ok;
}

namespace {
struct HashKey {
    uint8_t h[20];
    bool operator==(const HashKey& o) const { return std::memcmp(h, o.h, 20) == 0; }
};
struct HashKeyHash {
    size_t operator()(const HashKey& k) const { size_t v; std::memcpy(&v, k.h, sizeof v); return v; }
};
}

bool GameKeys::searchExecutable(const QString& exePath, const Progress& progress, QString* err)
{
    QFile f(exePath);
    if (!f.open(QIODevice::ReadOnly)) {
        if (err) *err = QStringLiteral("Cannot open %1").arg(exePath);
        return false;
    }
    const qint64 size = f.size();
    const uchar* data = f.map(0, size);
    QByteArray fallback;
    if (!data) {
        fallback = f.readAll();
        data = reinterpret_cast<const uchar*>(fallback.constData());
    }

    struct Target { int length; std::unordered_map<HashKey, int, HashKeyHash> hashes; };
    Target tAes{32, {}}, tNg{NgKeySize, {}}, tTab{1024, {}}, tLut{256, {}};
    auto add = [](Target& t, const uint8_t* h, int idx) { HashKey k; std::memcpy(k.h, h, 20); t.hashes[k] = idx; };
    add(tAes, keyhashes::AES_KEY_HASH, 0);
    add(tLut, keyhashes::LUT_HASH, 0);
    for (int i = 0; i < NgKeyCount; ++i) add(tNg, keyhashes::NG_KEY_HASHES[i], i);
    for (int i = 0; i < NgTableCount; ++i) add(tTab, keyhashes::NG_DECRYPT_TABLE_HASHES[i], i);
    Target* targets[] = {&tAes, &tNg, &tTab, &tLut};

    QByteArray foundAes, foundLut;
    std::vector<QByteArray> foundNg(NgKeyCount), foundTab(NgTableCount);
    std::mutex mtx;
    std::atomic<qint64> next{0};
    std::atomic<int> doneChunks{0};
    const qint64 chunk = 1 << 20;
    const qint64 chunks = (size + chunk - 1) / chunk;
    unsigned nthreads = std::max(1u, std::thread::hardware_concurrency());

    if (progress) progress(QStringLiteral("Searching %1 MB with %2 threads...").arg(size >> 20).arg(nthreads));
    auto worker = [&]() {
        uint8_t digest[20];
        for (;;) {
            qint64 c = next.fetch_add(1);
            if (c >= chunks) break;
            qint64 begin = c * chunk, end = std::min(size, begin + chunk);
            for (qint64 pos = begin; pos < end; pos += 8) {
                for (Target* t : targets) {
                    if (pos + t->length > size) continue;
                    SHA1(data + pos, size_t(t->length), digest);
                    HashKey k; std::memcpy(k.h, digest, 20);
                    auto it = t->hashes.find(k);
                    if (it == t->hashes.end()) continue;
                    QByteArray hit(reinterpret_cast<const char*>(data + pos), t->length);
                    std::lock_guard<std::mutex> lk(mtx);
                    if (t == &tAes) foundAes = hit;
                    else if (t == &tLut) foundLut = hit;
                    else if (t == &tNg) foundNg[it->second] = hit;
                    else foundTab[it->second] = hit;
                }
            }
            doneChunks.fetch_add(1);
        }
    };
    std::vector<std::thread> threads;
    for (unsigned i = 0; i < nthreads; ++i) threads.emplace_back(worker);
    if (progress) {
        while (doneChunks.load() < chunks) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            progress(QStringLiteral("Searching... %1%").arg(doneChunks.load() * 100 / std::max<qint64>(1, chunks)));
        }
    }
    for (auto& t : threads) t.join();

    if (foundAes.size() == 32) aesKey = foundAes;
    if (foundLut.size() == 256) hashLut = foundLut;
    bool allNg = true;
    for (auto& k : foundNg) allNg &= (k.size() == NgKeySize);
    if (allNg) {
        QByteArray b;
        for (auto& k : foundNg) b += k;
        setNgKeys(*this, b);
    }
    bool allTab = true;
    for (auto& t : foundTab) allTab &= (t.size() == 1024);
    if (allTab) {
        QByteArray b;
        for (auto& t : foundTab) b += t;
        setNgTables(*this, b);
    }
    if (progress) progress(QStringLiteral("Search finished. %1").arg(summary()));
    return true;
}

bool GameKeys::loadMagicDat(const QString& path, QString* err)
{
    if (!hasAes()) {
        if (err) *err = QStringLiteral("The AES key is needed to read magic.dat (search GTA5.exe first)");
        return false;
    }
    QByteArray m = readAll(path);
    if (m.isEmpty()) {
        if (err) *err = QStringLiteral("Cannot read %1").arg(path);
        return false;
    }
    DotNetRandom rnd(int32_t(joaat(aesKey)));
    const size_t n = size_t(m.size());
    std::vector<uint8_t> r1(n), r2(n), r3(n), r4(n);
    rnd.nextBytes(r1.data(), n);
    rnd.nextBytes(r2.data(), n);
    rnd.nextBytes(r3.data(), n);
    rnd.nextBytes(r4.data(), n);
    for (size_t i = 0; i < n; ++i)
        m[int(i)] = char(uint8_t(m[int(i)]) - r1[i] - r2[i] - r3[i] - r4[i]);
    aesDecrypt(m, aesKey);
    QByteArray b;
    if (!inflateRaw(m, b, -1) || b.size() < NgKeyCount * NgKeySize + NgTablesSize + 256) {
        if (err) *err = QStringLiteral("magic.dat could not be decoded (wrong AES key or file?)");
        return false;
    }
    setNgKeys(*this, b.left(NgKeyCount * NgKeySize));
    setNgTables(*this, b.mid(NgKeyCount * NgKeySize, NgTablesSize));
    hashLut = b.mid(NgKeyCount * NgKeySize + NgTablesSize, 256);
    return true;
}

// ---------------------------------------------------------------- hashes

uint32_t joaat(const QByteArray& s)
{
    uint32_t h = 0;
    for (char c : s) {
        h += uint8_t(c);
        h += h << 10;
        h ^= h >> 6;
    }
    h += h << 3;
    h ^= h >> 11;
    h += h << 15;
    return h;
}

uint32_t joaatLower(const QString& s) { return joaat(s.toLower().toUtf8()); }

uint32_t ngNameHash(const QString& name, const GameKeys& keys)
{
    QByteArray b = name.toLatin1();
    uint32_t result = 0;
    for (char ch : b) {
        uint8_t c = uint8_t(ch);
        uint32_t m;
        if (keys.hasLut()) m = uint8_t(keys.hashLut[c]);
        else { m = (c >= 'A' && c <= 'Z') ? c + 32 : (c == '\\' ? '/' : c); }
        uint32_t temp = 1025u * (m + result);
        result = (temp >> 6) ^ temp;
    }
    return 32769u * ((9u * result >> 11) ^ (9u * result));
}

// ---------------------------------------------------------------- AES

static bool aesEcb(QByteArray& data, const QByteArray& key, bool enc)
{
    if (key.size() != 32) return false;
    int len = data.size() - data.size() % 16;
    if (len <= 0) return true;
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return false;
    bool ok = EVP_CipherInit_ex(ctx, EVP_aes_256_ecb(), nullptr,
                                reinterpret_cast<const unsigned char*>(key.constData()), nullptr, enc ? 1 : 0) == 1;
    EVP_CIPHER_CTX_set_padding(ctx, 0);
    QByteArray out(len + 32, 0);
    int outl = 0;
    ok = ok && EVP_CipherUpdate(ctx, reinterpret_cast<unsigned char*>(out.data()), &outl,
                                reinterpret_cast<const unsigned char*>(data.constData()), len) == 1;
    EVP_CIPHER_CTX_free(ctx);
    if (ok) std::memcpy(data.data(), out.constData(), size_t(len));
    return ok;
}

bool aesDecrypt(QByteArray& data, const QByteArray& key) { return aesEcb(data, key, false); }
bool aesEncrypt(QByteArray& data, const QByteArray& key) { return aesEcb(data, key, true); }

// ---------------------------------------------------------------- NG

static inline void ngRoundA(uint8_t* b, const uint32_t* key, const uint32_t* tab)
{
    auto T = [tab](int t, uint8_t v) { return tab[t * 256 + v]; };
    uint32_t x1 = T(0, b[0]) ^ T(1, b[1]) ^ T(2, b[2]) ^ T(3, b[3]) ^ key[0];
    uint32_t x2 = T(4, b[4]) ^ T(5, b[5]) ^ T(6, b[6]) ^ T(7, b[7]) ^ key[1];
    uint32_t x3 = T(8, b[8]) ^ T(9, b[9]) ^ T(10, b[10]) ^ T(11, b[11]) ^ key[2];
    uint32_t x4 = T(12, b[12]) ^ T(13, b[13]) ^ T(14, b[14]) ^ T(15, b[15]) ^ key[3];
    std::memcpy(b, &x1, 4); std::memcpy(b + 4, &x2, 4); std::memcpy(b + 8, &x3, 4); std::memcpy(b + 12, &x4, 4);
}

static inline void ngRoundB(uint8_t* b, const uint32_t* key, const uint32_t* tab)
{
    auto T = [tab](int t, uint8_t v) { return tab[t * 256 + v]; };
    uint32_t x1 = T(0, b[0]) ^ T(7, b[7]) ^ T(10, b[10]) ^ T(13, b[13]) ^ key[0];
    uint32_t x2 = T(1, b[1]) ^ T(4, b[4]) ^ T(11, b[11]) ^ T(14, b[14]) ^ key[1];
    uint32_t x3 = T(2, b[2]) ^ T(5, b[5]) ^ T(8, b[8]) ^ T(15, b[15]) ^ key[2];
    uint32_t x4 = T(3, b[3]) ^ T(6, b[6]) ^ T(9, b[9]) ^ T(12, b[12]) ^ key[3];
    std::memcpy(b, &x1, 4); std::memcpy(b + 4, &x2, 4); std::memcpy(b + 8, &x3, 4); std::memcpy(b + 12, &x4, 4);
}

void ngDecryptWithKey(QByteArray& data, const uint32_t* key, const GameKeys& keys)
{
    const uint32_t* tables = keys.ngTables.data();
    uint8_t* p = reinterpret_cast<uint8_t*>(data.data());
    const int blocks = data.size() / 16;
    for (int i = 0; i < blocks; ++i, p += 16) {
        ngRoundA(p, key + 0, tables + 0 * 16 * 256);
        ngRoundA(p, key + 4, tables + 1 * 16 * 256);
        for (int k = 2; k <= 15; ++k) ngRoundB(p, key + 4 * k, tables + k * 16 * 256);
        ngRoundA(p, key + 64, tables + 16 * 16 * 256);
    }
}

bool ngDecrypt(QByteArray& data, const QString& name, uint32_t length, const GameKeys& keys)
{
    if (!keys.hasNg()) return false;
    uint32_t idx = (ngNameHash(name, keys) + length + (101 - 40)) % 101;
    ngDecryptWithKey(data, keys.ngKeys[idx].data(), keys);
    return true;
}

// ---------------------------------------------------------------- .NET Random

DotNetRandom::DotNetRandom(int32_t seed)
{
    const int32_t MBIG = 0x7fffffff, MSEED = 161803398;
    int32_t subtraction = (seed == INT32_MIN) ? INT32_MAX : (seed < 0 ? -seed : seed);
    int32_t mj = MSEED - subtraction;
    m_seed[55] = mj;
    int32_t mk = 1;
    for (int i = 1; i < 55; ++i) {
        int ii = (21 * i) % 55;
        m_seed[ii] = mk;
        mk = int32_t(uint32_t(mj) - uint32_t(mk));
        if (mk < 0) mk += MBIG;
        mj = m_seed[ii];
    }
    for (int k = 1; k < 5; ++k) {
        for (int i = 1; i < 56; ++i) {
            m_seed[i] = int32_t(uint32_t(m_seed[i]) - uint32_t(m_seed[1 + (i + 30) % 55]));
            if (m_seed[i] < 0) m_seed[i] += MBIG;
        }
    }
    m_seed[0] = 0;
}

int32_t DotNetRandom::sample()
{
    const int32_t MBIG = 0x7fffffff;
    int a = m_inext, b = m_inextp;
    if (++a >= 56) a = 1;
    if (++b >= 56) b = 1;
    int32_t r = int32_t(uint32_t(m_seed[a]) - uint32_t(m_seed[b]));
    if (r == MBIG) r--;
    if (r < 0) r += MBIG;
    m_seed[a] = r;
    m_inext = a;
    m_inextp = b;
    return r;
}

void DotNetRandom::nextBytes(uint8_t* buf, size_t n)
{
    for (size_t i = 0; i < n; ++i) buf[i] = uint8_t(sample() & 0xFF);
}

} // namespace rs
