#include "TpfArchive.h"

#include <algorithm>
#include <map>
#include <string.h>
#include <zlib.h>

namespace {

static const unsigned char kTpfPassword[] = {
    0x73, 0x2A, 0x63, 0x7D, 0x5F, 0x0A, 0xA6, 0xBD,
    0x7D, 0x65, 0x7E, 0x67, 0x61, 0x2A, 0x7F, 0x7F,
    0x74, 0x61, 0x67, 0x5B, 0x60, 0x70, 0x45, 0x74,
    0x5C, 0x22, 0x74, 0x5D, 0x6E, 0x6A, 0x73, 0x41,
    0x77, 0x6E, 0x46, 0x47, 0x77, 0x49, 0x0C, 0x4B,
    0x46, 0x6F, 0x00
};

static uint16_t Read16(const unsigned char* p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t Read32(const unsigned char* p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static bool ReadWholeFile(const std::wstring& path, std::vector<unsigned char>& out, std::wstring& error)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = L"CreateFileW failed, error=" + std::to_wstring(GetLastError());
        return false;
    }

    LARGE_INTEGER size = {};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > 0x7FFFFFFFLL) {
        error = L"Invalid or unsupported package size";
        CloseHandle(file);
        return false;
    }

    out.resize((size_t)size.QuadPart);
    DWORD total = 0;
    while (total < out.size()) {
        DWORD chunk = (DWORD)std::min<size_t>(out.size() - total, 0x40000000u);
        DWORD got = 0;
        if (!ReadFile(file, out.data() + total, chunk, &got, nullptr) || got == 0) {
            error = L"ReadFile failed, error=" + std::to_wstring(GetLastError());
            CloseHandle(file);
            return false;
        }
        total += got;
    }

    CloseHandle(file);
    return true;
}

static bool EndsWithInsensitive(const std::wstring& value, const wchar_t* suffix)
{
    size_t n = wcslen(suffix);
    if (value.size() < n) return false;
    return _wcsicmp(value.c_str() + value.size() - n, suffix) == 0;
}

static void DecryptTpfContainer(std::vector<unsigned char>& data)
{
    const uint32_t key = 0x3FA43FA4u;
    size_t words = data.size() / 4;
    for (size_t i = 0; i < words; ++i) {
        uint32_t v = Read32(data.data() + i * 4) ^ key;
        data[i * 4 + 0] = (unsigned char)(v & 0xFF);
        data[i * 4 + 1] = (unsigned char)((v >> 8) & 0xFF);
        data[i * 4 + 2] = (unsigned char)((v >> 16) & 0xFF);
        data[i * 4 + 3] = (unsigned char)((v >> 24) & 0xFF);
    }
    for (size_t i = words * 4; i < data.size(); ++i)
        data[i] ^= (unsigned char)key;

    // Original TexMod may append author/comment data after a NUL separator.
    // Match the classic loader's behavior and trim to the final NUL-delimited ZIP end.
    if (data.size() > 1) {
        size_t pos = data.size() - 1;
        while (pos > 0 && data[pos] != 0) --pos;
        if (pos > 0 && pos < data.size() - 1)
            data.resize(pos + 1);
    }
}

struct ZipEntry {
    std::string name;
    uint16_t flags = 0;
    uint16_t method = 0;
    uint16_t modTime = 0;
    uint32_t crc = 0;
    uint32_t compressedSize = 0;
    uint32_t uncompressedSize = 0;
    uint32_t localOffset = 0;
};

static std::string LowerAscii(std::string s)
{
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = (char)(c + ('a' - 'A'));
    }
    return s;
}

static bool ParseCentralDirectory(const std::vector<unsigned char>& zip,
                                  std::vector<ZipEntry>& entries,
                                  std::wstring& error)
{
    if (zip.size() < 22) {
        error = L"Package is too small to be a ZIP archive";
        return false;
    }

    const uint32_t eocdSig = 0x06054B50u;
    size_t searchStart = zip.size() > (0xFFFFu + 22u) ? zip.size() - (0xFFFFu + 22u) : 0;
    size_t eocd = SIZE_MAX;
    for (size_t p = zip.size() - 22; ; --p) {
        if (Read32(zip.data() + p) == eocdSig) {
            eocd = p;
            break;
        }
        if (p == searchStart) break;
    }
    if (eocd == SIZE_MAX) {
        error = L"ZIP end-of-central-directory record not found";
        return false;
    }

    uint16_t diskNo = Read16(zip.data() + eocd + 4);
    uint16_t cdDisk = Read16(zip.data() + eocd + 6);
    uint16_t count = Read16(zip.data() + eocd + 10);
    uint32_t cdSize = Read32(zip.data() + eocd + 12);
    uint32_t cdOffset = Read32(zip.data() + eocd + 16);
    if (diskNo != 0 || cdDisk != 0) {
        error = L"Multi-disk ZIP archives are not supported";
        return false;
    }
    if ((uint64_t)cdOffset + cdSize > zip.size()) {
        error = L"ZIP central directory lies outside the package";
        return false;
    }

    size_t p = cdOffset;
    entries.reserve(count);
    for (uint16_t i = 0; i < count; ++i) {
        if (p + 46 > zip.size() || Read32(zip.data() + p) != 0x02014B50u) {
            error = L"Invalid ZIP central-directory entry";
            return false;
        }

        ZipEntry e;
        e.flags = Read16(zip.data() + p + 8);
        e.method = Read16(zip.data() + p + 10);
        e.modTime = Read16(zip.data() + p + 12);
        e.crc = Read32(zip.data() + p + 16);
        e.compressedSize = Read32(zip.data() + p + 20);
        e.uncompressedSize = Read32(zip.data() + p + 24);
        uint16_t nameLen = Read16(zip.data() + p + 28);
        uint16_t extraLen = Read16(zip.data() + p + 30);
        uint16_t commentLen = Read16(zip.data() + p + 32);
        e.localOffset = Read32(zip.data() + p + 42);

        if (p + 46u + nameLen + extraLen + commentLen > zip.size()) {
            error = L"Truncated ZIP central-directory entry";
            return false;
        }
        e.name.assign((const char*)zip.data() + p + 46, nameLen);
        entries.push_back(e);
        p += 46u + nameLen + extraLen + commentLen;
    }
    return true;
}

static uint32_t g_crcTable[256];
static bool g_crcReady = false;

static void EnsureZipCryptoCrcTable()
{
    if (g_crcReady) return;
    for (uint32_t n = 0; n < 256; ++n) {
        uint32_t c = n;
        for (int k = 0; k < 8; ++k)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        g_crcTable[n] = c;
    }
    g_crcReady = true;
}

static uint32_t ZipCryptoCrc32(uint32_t oldCrc, unsigned char value)
{
    return g_crcTable[(oldCrc ^ value) & 0xFF] ^ (oldCrc >> 8);
}

struct ZipCryptoKeys {
    uint32_t key0 = 305419896u;
    uint32_t key1 = 591751049u;
    uint32_t key2 = 878082192u;
};

static void ZipCryptoUpdate(ZipCryptoKeys& k, unsigned char plain)
{
    k.key0 = ZipCryptoCrc32(k.key0, plain);
    k.key1 = (k.key1 + (k.key0 & 0xFFu)) * 134775813u + 1u;
    k.key2 = ZipCryptoCrc32(k.key2, (unsigned char)(k.key1 >> 24));
}

static unsigned char ZipCryptoByte(const ZipCryptoKeys& k)
{
    uint16_t t = (uint16_t)(k.key2 | 2u);
    return (unsigned char)(((uint32_t)t * (uint32_t)(t ^ 1u)) >> 8);
}

static void ZipCryptoInit(ZipCryptoKeys& k, const unsigned char* password)
{
    EnsureZipCryptoCrcTable();
    for (const unsigned char* p = password; *p; ++p)
        ZipCryptoUpdate(k, *p);
}

static void ZipCryptoDecrypt(ZipCryptoKeys& k,
                             const unsigned char* input,
                             size_t size,
                             std::vector<unsigned char>& output)
{
    output.resize(size);
    for (size_t i = 0; i < size; ++i) {
        unsigned char plain = input[i] ^ ZipCryptoByte(k);
        ZipCryptoUpdate(k, plain);
        output[i] = plain;
    }
}

static bool InflateRaw(const unsigned char* input, size_t inputSize,
                       std::vector<unsigned char>& output, size_t outputSize,
                       std::wstring& error)
{
    output.assign(outputSize, 0);
    z_stream s = {};
    s.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(input));
    s.avail_in = (uInt)inputSize;
    s.next_out = reinterpret_cast<Bytef*>(output.data());
    s.avail_out = (uInt)output.size();

    int zr = inflateInit2(&s, -MAX_WBITS);
    if (zr != Z_OK) {
        error = L"zlib inflateInit2 failed: " + std::to_wstring(zr);
        return false;
    }
    zr = inflate(&s, Z_FINISH);
    inflateEnd(&s);
    if (zr != Z_STREAM_END || s.total_out != outputSize) {
        error = L"zlib raw-deflate decompression failed: " + std::to_wstring(zr);
        return false;
    }
    return true;
}

static bool ExtractEntry(const std::vector<unsigned char>& zip,
                         const ZipEntry& e,
                         bool useTpfPassword,
                         std::vector<unsigned char>& output,
                         std::wstring& error)
{
    if ((uint64_t)e.localOffset + 30 > zip.size() ||
        Read32(zip.data() + e.localOffset) != 0x04034B50u) {
        error = L"Invalid ZIP local-file header";
        return false;
    }

    const unsigned char* local = zip.data() + e.localOffset;
    uint16_t nameLen = Read16(local + 26);
    uint16_t extraLen = Read16(local + 28);
    size_t dataOffset = (size_t)e.localOffset + 30u + nameLen + extraLen;
    if ((uint64_t)dataOffset + e.compressedSize > zip.size()) {
        error = L"Compressed ZIP entry lies outside the package";
        return false;
    }

    const unsigned char* compressed = zip.data() + dataOffset;
    size_t compressedSize = e.compressedSize;
    std::vector<unsigned char> decrypted;

    if (e.flags & 1u) {
        if (!useTpfPassword) {
            error = L"Encrypted ZIP entry encountered without a TPF password";
            return false;
        }
        if (compressedSize < 12) {
            error = L"Truncated traditional ZIP encryption header";
            return false;
        }
        ZipCryptoKeys keys;
        ZipCryptoInit(keys, kTpfPassword);
        ZipCryptoDecrypt(keys, compressed, compressedSize, decrypted);

        // Traditional encryption prepends 12 decrypted verification bytes.
        unsigned char expected = (e.flags & 8u)
            ? (unsigned char)(e.modTime >> 8)
            : (unsigned char)(e.crc >> 24);
        // Some historical TPF writers are inconsistent about the verification byte.
        // Continue if it differs and let decompression/CRC validation decide validity.
        (void)expected;
        compressed = decrypted.data() + 12;
        compressedSize = decrypted.size() - 12;
    }

    if (e.method == 0) {
        if (compressedSize != e.uncompressedSize) {
            error = L"Stored ZIP entry has an unexpected size";
            return false;
        }
        output.assign(compressed, compressed + compressedSize);
    } else if (e.method == 8) {
        if (!InflateRaw(compressed, compressedSize, output, e.uncompressedSize, error))
            return false;
    } else {
        error = L"Unsupported ZIP compression method: " + std::to_wstring(e.method);
        return false;
    }

    uLong crc = crc32(0L, Z_NULL, 0);
    if (!output.empty()) crc = crc32(crc, output.data(), (uInt)output.size());
    if ((uint32_t)crc != e.crc) {
        error = L"ZIP entry CRC mismatch";
        return false;
    }
    return true;
}

static void DetectImageSize(TpfTextureEntry& tex)
{
    const std::vector<unsigned char>& d = tex.imageData;
    // PNG signature + IHDR width/height (big endian).
    if (d.size() >= 24 &&
        d[0] == 0x89 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G' &&
        d[12] == 'I' && d[13] == 'H' && d[14] == 'D' && d[15] == 'R') {
        tex.imageWidth = ((uint32_t)d[16] << 24) | ((uint32_t)d[17] << 16) |
                         ((uint32_t)d[18] << 8) | d[19];
        tex.imageHeight = ((uint32_t)d[20] << 24) | ((uint32_t)d[21] << 16) |
                          ((uint32_t)d[22] << 8) | d[23];
        return;
    }
    // DDS: magic + DDS_HEADER dwHeight/dwWidth.
    if (d.size() >= 20 && d[0] == 'D' && d[1] == 'D' && d[2] == 'S' && d[3] == ' ') {
        tex.imageHeight = Read32(d.data() + 12);
        tex.imageWidth = Read32(d.data() + 16);
        return;
    }
    // BMP width/height.
    if (d.size() >= 26 && d[0] == 'B' && d[1] == 'M') {
        tex.imageWidth = Read32(d.data() + 18);
        tex.imageHeight = Read32(d.data() + 22);
    }
}

static bool ParseHash(const std::string& text, uint32_t& out)
{
    char* end = nullptr;
    unsigned long v = strtoul(text.c_str(), &end, 0);
    if (!end || end == text.c_str() || v > 0xFFFFFFFFul) return false;
    out = (uint32_t)v;
    return out != 0;
}

} // namespace

TpfLoadResult LoadTpfOrZipPackage(const std::wstring& path)
{
    TpfLoadResult result;
    std::vector<unsigned char> zip;
    if (!ReadWholeFile(path, zip, result.error)) return result;

    bool isTpf = EndsWithInsensitive(path, L".tpf");
    if (isTpf) DecryptTpfContainer(zip);

    std::vector<ZipEntry> entries;
    if (!ParseCentralDirectory(zip, entries, result.error)) return result;

    std::map<std::string, size_t> byName;
    for (size_t i = 0; i < entries.size(); ++i)
        byName[LowerAscii(entries[i].name)] = i;

    auto defIt = byName.find("texmod.def");
    if (defIt == byName.end()) {
        result.error = L"texmod.def was not found in the package";
        return result;
    }

    std::vector<unsigned char> defBytes;
    if (!ExtractEntry(zip, entries[defIt->second], isTpf, defBytes, result.error))
        return result;
    defBytes.push_back(0);

    std::string def((const char*)defBytes.data());
    size_t pos = 0;
    while (pos < def.size()) {
        size_t end = def.find_first_of("\r\n", pos);
        if (end == std::string::npos) end = def.size();
        std::string line = def.substr(pos, end - pos);
        while (end < def.size() && (def[end] == '\r' || def[end] == '\n')) ++end;
        pos = end;
        if (line.empty()) continue;

        size_t pipe = line.find('|');
        if (pipe == std::string::npos) continue;
        uint32_t hash = 0;
        if (!ParseHash(line.substr(0, pipe), hash)) continue;
        std::string fileName = line.substr(pipe + 1);
        if (fileName.empty()) continue;

        auto fileIt = byName.find(LowerAscii(fileName));
        if (fileIt == byName.end()) continue;

        TpfTextureEntry tex;
        tex.hash = hash;
        tex.internalName = fileName;
        if (!ExtractEntry(zip, entries[fileIt->second], isTpf, tex.imageData, result.error))
            return result;
        DetectImageSize(tex);
        result.textures.push_back(std::move(tex));
    }

    if (result.textures.empty()) {
        result.error = L"texmod.def did not yield any usable texture entries";
        return result;
    }

    result.ok = true;
    return result;
}
