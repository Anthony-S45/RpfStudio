// GTA V PC cryptography: AES (legacy archives), NG (most PC archives) and
// the name hashes used by the engine. Keys are never shipped with this
// program: they are located in the user's own copy of the game.
#pragma once
#include <QByteArray>
#include <QString>
#include <array>
#include <cstdint>
#include <functional>
#include <vector>

namespace rs {

class GameKeys {
public:
    static constexpr int NgKeyCount = 101;
    static constexpr int NgKeySize = 272;          // 17 round keys * 16 bytes
    static constexpr int NgTableCount = 17 * 16;   // 17 rounds * 16 tables
    static constexpr int NgTablesSize = NgTableCount * 256 * 4;

    QByteArray aesKey;                  // 32 bytes
    std::vector<std::array<uint32_t, 68>> ngKeys;  // 101 keys
    std::vector<uint32_t> ngTables;     // 17*16*256 entries
    QByteArray hashLut;                 // 256 bytes

    bool hasAes() const { return aesKey.size() == 32; }
    bool hasNg() const { return ngKeys.size() == NgKeyCount && ngTables.size() == size_t(NgTableCount) * 256; }
    bool hasLut() const { return hashLut.size() == 256; }
    bool complete() const { return hasAes() && hasNg() && hasLut(); }
    QString summary() const;

    // Load/save the common key-file layout used by most GTA V tools:
    // gtav_aes_key.dat, gtav_ng_key.dat, gtav_ng_decrypt_tables.dat, gtav_hash_lut.dat
    bool loadFromDir(const QString& dir, QString* err);
    bool saveToDir(const QString& dir, QString* err) const;

    // Search the user's GTA5.exe for key material (by SHA-1 of every aligned
    // block). Finds whatever is present; returns false only on I/O failure.
    using Progress = std::function<void(const QString&)>;
    bool searchExecutable(const QString& exePath, const Progress& progress, QString* err);

    // CodeWalker-compatible "magic.dat" (NG keys, tables and LUT, encrypted
    // with the AES key). Requires aesKey to be set first.
    bool loadMagicDat(const QString& path, QString* err);

    // Global key store used by archive code.
    static GameKeys& global();
};

// Engine hashes
uint32_t joaat(const QByteArray& lowercaseAscii);   // Jenkins one-at-a-time
uint32_t joaatLower(const QString& s);               // lowercases first
uint32_t ngNameHash(const QString& name, const GameKeys& keys); // uses hash LUT

// AES-256-ECB without padding; trailing partial block left untouched.
bool aesDecrypt(QByteArray& data, const QByteArray& key);
bool aesEncrypt(QByteArray& data, const QByteArray& key);

// NG decryption. Key is chosen from the file name and length.
bool ngDecrypt(QByteArray& data, const QString& name, uint32_t length, const GameKeys& keys);
void ngDecryptWithKey(QByteArray& data, const uint32_t* key68, const GameKeys& keys);

// .NET System.Random compatible generator (seeded), needed for magic.dat.
class DotNetRandom {
public:
    explicit DotNetRandom(int32_t seed);
    int32_t sample();
    void nextBytes(uint8_t* buf, size_t n);
private:
    int32_t m_seed[56];
    int m_inext = 0, m_inextp = 21;
};

} // namespace rs
