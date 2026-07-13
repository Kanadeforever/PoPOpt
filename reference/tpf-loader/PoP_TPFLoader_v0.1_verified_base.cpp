// ============================================================================
// PoP_TPFLoader.asi - experimental standalone TexMod-compatible TPF loader
// for Prince of Persia (2008), Direct3D 9, 32-bit.
//
// Runtime scope:
//   - Reads only explicitly configured .tpf/.zip packages.
//   - Does not use TexMod.exe, tmldr.dll, tmrls.dll, or the TexMod registry key.
//   - Hooks the game's imported Direct3DCreate9 and selected D3D9 vtable calls.
//   - Computes several CRC32 candidates for original textures and replaces a
//     matched texture using image bytes loaded from the package.
//
// This is an EXPERIMENTAL validation plugin. Keep it separate from PoPOpt until
// its hash compatibility and D3D9 lifecycle behavior are confirmed in game.
// Delete the ASI to remove all of its behavior.
//
// Build requirements:
//   - 32-bit MSVC /MT
//   - Windows SDK / Direct3D 9 headers
//   - zlib linked statically into the ASI
//   - No static dependency on D3DX: d3dx9_*.dll is resolved dynamically.
// ============================================================================

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <wchar.h>

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <zlib.h>

#include "TpfArchive.h"

// ------------------------------ Global paths -------------------------------
static HMODULE g_module = nullptr;
static wchar_t g_modulePath[MAX_PATH * 4] = {};
static wchar_t g_baseDir[MAX_PATH * 4] = {};
static wchar_t g_stem[MAX_PATH * 4] = {};
static wchar_t g_iniPath[MAX_PATH * 4] = {};
static wchar_t g_logPath[MAX_PATH * 4] = {};
static char g_logPrefix[512] = "[PoP_TPFLoader] ";
static int g_logLevel = 2;

// ----------------------------- Configuration -------------------------------
static bool g_enabled = true;
static bool g_laterPackageWins = true;
static bool g_onlyTargetDimensions = true;
static int g_maxHashDimension = 512;
static int g_maxUnmatchedLogs = 80;
static bool g_logUnmatched = false;

// ---------------------------- Package database -----------------------------
struct ReplacementEntry {
    uint32_t hash = 0;
    std::wstring packagePath;
    std::string internalName;
    std::vector<unsigned char> imageData;
    uint32_t imageWidth = 0;
    uint32_t imageHeight = 0;
    int packageOrder = 0;
};

static std::unordered_map<uint32_t, ReplacementEntry> g_replacements;
static std::set<uint64_t> g_targetDimensions;
static int g_loadedPackageCount = 0;
static int g_duplicateDefinitionCount = 0;

// ------------------------------- D3DX loader -------------------------------
#define D3DX_DEFAULT_LOCAL 0xFFFFFFFFu
#define D3DX_FILTER_DEFAULT_LOCAL 0xFFFFFFFFu

typedef HRESULT (WINAPI *D3DXCreateTextureFromFileInMemoryEx_t)(
    IDirect3DDevice9* device,
    LPCVOID sourceData,
    UINT sourceDataSize,
    UINT width,
    UINT height,
    UINT mipLevels,
    DWORD usage,
    D3DFORMAT format,
    D3DPOOL pool,
    DWORD filter,
    DWORD mipFilter,
    D3DCOLOR colorKey,
    void* sourceInfo,
    PALETTEENTRY* palette,
    IDirect3DTexture9** textureOut);

static HMODULE g_d3dxModule = nullptr;
static D3DXCreateTextureFromFileInMemoryEx_t g_D3DXCreateTextureFromFileInMemoryEx = nullptr;

// ----------------------------- Runtime textures ----------------------------
struct RuntimeTexture {
    IDirect3DTexture9* replacement = nullptr;
    uint32_t matchedHash = 0;
    bool dirty = true;
    bool attempted = false;
};

static CRITICAL_SECTION g_textureLock;
static bool g_textureLockReady = false;
static std::unordered_map<IDirect3DTexture9*, RuntimeTexture> g_runtimeTextures;
static volatile LONG g_internalTextureCreate = 0;
static volatile LONG g_unmatchedLogCount = 0;
static IDirect3DDevice9* g_lastDevice = nullptr; // non-owning

// ------------------------------ D3D pointers --------------------------------
typedef IDirect3D9* (WINAPI *Direct3DCreate9_t)(UINT SDKVersion);
typedef HRESULT (STDMETHODCALLTYPE *D3D9_CreateDevice_t)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
typedef ULONG (STDMETHODCALLTYPE *Device_Release_t)(IDirect3DDevice9*);
typedef HRESULT (STDMETHODCALLTYPE *Device_Reset_t)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
typedef HRESULT (STDMETHODCALLTYPE *Device_CreateTexture_t)(IDirect3DDevice9*, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DTexture9**, HANDLE*);
typedef HRESULT (STDMETHODCALLTYPE *Device_UpdateTexture_t)(IDirect3DDevice9*, IDirect3DBaseTexture9*, IDirect3DBaseTexture9*);
typedef HRESULT (STDMETHODCALLTYPE *Device_SetTexture_t)(IDirect3DDevice9*, DWORD, IDirect3DBaseTexture9*);
typedef ULONG (STDMETHODCALLTYPE *Texture_Release_t)(IDirect3DTexture9*);
typedef HRESULT (STDMETHODCALLTYPE *Texture_LockRect_t)(IDirect3DTexture9*, UINT, D3DLOCKED_RECT*, const RECT*, DWORD);
typedef HRESULT (STDMETHODCALLTYPE *Texture_UnlockRect_t)(IDirect3DTexture9*, UINT);
typedef HRESULT (STDMETHODCALLTYPE *Texture_AddDirtyRect_t)(IDirect3DTexture9*, const RECT*);

static Direct3DCreate9_t Real_Direct3DCreate9 = nullptr;
static D3D9_CreateDevice_t Real_D3D9_CreateDevice = nullptr;
static Device_Release_t Real_Device_Release = nullptr;
static Device_Reset_t Real_Device_Reset = nullptr;
static Device_CreateTexture_t Real_Device_CreateTexture = nullptr;
static Device_UpdateTexture_t Real_Device_UpdateTexture = nullptr;
static Device_SetTexture_t Real_Device_SetTexture = nullptr;
static Texture_Release_t Real_Texture_Release = nullptr;
static Texture_LockRect_t Real_Texture_LockRect = nullptr;
static Texture_UnlockRect_t Real_Texture_UnlockRect = nullptr;
static Texture_AddDirtyRect_t Real_Texture_AddDirtyRect = nullptr;

// D3D9 vtable indices for the non-Ex interfaces.
static const int VTBL_D3D9_CREATE_DEVICE = 16;
static const int VTBL_DEVICE_RELEASE = 2;
static const int VTBL_DEVICE_RESET = 16;
static const int VTBL_DEVICE_CREATE_TEXTURE = 23;
static const int VTBL_DEVICE_UPDATE_TEXTURE = 31;
static const int VTBL_DEVICE_SET_TEXTURE = 65;
static const int VTBL_TEXTURE_RELEASE = 2;
static const int VTBL_TEXTURE_LOCK_RECT = 19;
static const int VTBL_TEXTURE_UNLOCK_RECT = 20;
static const int VTBL_TEXTURE_ADD_DIRTY_RECT = 21;

// ------------------------------- Logging -----------------------------------
static void WideToUtf8(const wchar_t* source, char* dest, int destSize)
{
    if (!dest || destSize <= 0) return;
    dest[0] = 0;
    if (!source) return;
    WideCharToMultiByte(CP_UTF8, 0, source, -1, dest, destSize, nullptr, nullptr);
    dest[destSize - 1] = 0;
}

static void LogV(int level, const char* format, va_list args)
{
    if (g_logLevel < level || !g_logPath[0]) return;

    char body[1800] = {};
#if defined(_MSC_VER)
    _vsnprintf_s(body, sizeof(body), _TRUNCATE, format, args);
#else
    vsnprintf(body, sizeof(body), format, args);
#endif
    body[sizeof(body) - 1] = 0;

    char line[2300] = {};
#if defined(_MSC_VER)
    _snprintf_s(line, sizeof(line), _TRUNCATE, "%s%s\r\n", g_logPrefix, body);
#else
    snprintf(line, sizeof(line), "%s%s\r\n", g_logPrefix, body);
#endif

    HANDLE file = CreateFileW(g_logPath, FILE_APPEND_DATA,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(file, line, (DWORD)strlen(line), &written, nullptr);
        CloseHandle(file);
    }
    OutputDebugStringA(line);
}

static void Log(int level, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    LogV(level, format, args);
    va_end(args);
}

// ------------------------------- Paths/INI ---------------------------------
static void CopyWide(wchar_t* dst, size_t dstCount, const wchar_t* src)
{
    if (!dst || dstCount == 0) return;
#if defined(_MSC_VER)
    wcsncpy_s(dst, dstCount, src ? src : L"", _TRUNCATE);
#else
    wcsncpy(dst, src ? src : L"", dstCount - 1);
    dst[dstCount - 1] = 0;
#endif
}

static void AppendWide(wchar_t* dst, size_t dstCount, const wchar_t* src)
{
    size_t len = wcslen(dst);
    if (len >= dstCount - 1) return;
    CopyWide(dst + len, dstCount - len, src);
}

static bool IsAbsolutePath(const wchar_t* path)
{
    if (!path || !path[0]) return false;
    if ((path[0] && path[1] == L':') || (path[0] == L'\\' && path[1] == L'\\')) return true;
    return path[0] == L'/' || path[0] == L'\\';
}

static std::wstring ResolvePath(const wchar_t* configured)
{
    if (!configured || !configured[0]) return std::wstring();
    if (IsAbsolutePath(configured)) return configured;
    std::wstring full = g_baseDir;
    if (!full.empty() && full.back() != L'\\' && full.back() != L'/') full += L'\\';
    full += configured;
    return full;
}

static void BuildPaths()
{
    GetModuleFileNameW(g_module, g_modulePath, (DWORD)(sizeof(g_modulePath) / sizeof(g_modulePath[0])));
    CopyWide(g_baseDir, _countof(g_baseDir), g_modulePath);
    wchar_t* slash = wcsrchr(g_baseDir, L'\\');
    if (!slash) slash = wcsrchr(g_baseDir, L'/');
    if (slash) *slash = 0;

    const wchar_t* fileName = slash ? slash + 1 : g_modulePath;
    CopyWide(g_stem, _countof(g_stem), fileName);
    wchar_t* dot = wcsrchr(g_stem, L'.');
    if (dot) *dot = 0;

    CopyWide(g_iniPath, _countof(g_iniPath), g_baseDir);
    AppendWide(g_iniPath, _countof(g_iniPath), L"\\");
    AppendWide(g_iniPath, _countof(g_iniPath), g_stem);
    AppendWide(g_iniPath, _countof(g_iniPath), L".ini");

    CopyWide(g_logPath, _countof(g_logPath), g_baseDir);
    AppendWide(g_logPath, _countof(g_logPath), L"\\");
    AppendWide(g_logPath, _countof(g_logPath), g_stem);
    AppendWide(g_logPath, _countof(g_logPath), L".log");

    char stemUtf8[400] = {};
    WideToUtf8(g_stem, stemUtf8, sizeof(stemUtf8));
#if defined(_MSC_VER)
    _snprintf_s(g_logPrefix, sizeof(g_logPrefix), _TRUNCATE, "[%s] ", stemUtf8[0] ? stemUtf8 : "PoP_TPFLoader");
#else
    snprintf(g_logPrefix, sizeof(g_logPrefix), "[%s] ", stemUtf8[0] ? stemUtf8 : "PoP_TPFLoader");
#endif
}

static void CreateDefaultIniIfMissing()
{
    if (GetFileAttributesW(g_iniPath) != INVALID_FILE_ATTRIBUTES) return;

    const wchar_t* text =
        L"; Experimental standalone TPF texture loader for Direct3D 9.\r\n"
        L"; The INI/log filenames follow the actual ASI filename.\r\n"
        L"; Later packages can override earlier packages when the same hash exists.\r\n"
        L"\r\n"
        L"[TPFLoader]\r\n"
        L"Enable=1\r\n"
        L"\r\n"
        L"; 0=no log, 1=errors/warnings, 2=normal, 3=verbose hash diagnostics.\r\n"
        L"LogLevel=2\r\n"
        L"\r\n"
        L"; 0=first package wins, 1=later package wins.\r\n"
        L"LaterPackageWins=1\r\n"
        L"\r\n"
        L"; Hash only textures whose dimensions occur in loaded replacement images.\r\n"
        L"; Recommended for the first controller-icon test. Set 0 if no hashes match.\r\n"
        L"OnlyTargetDimensions=1\r\n"
        L"\r\n"
        L"; Textures larger than this are skipped when hashing. 0=unlimited.\r\n"
        L"MaxHashDimension=512\r\n"
        L"\r\n"
        L"; Verbose unmatched hash logging. Useful only while diagnosing hash compatibility.\r\n"
        L"LogUnmatched=0\r\n"
        L"MaxUnmatchedLogs=80\r\n"
        L"\r\n"
        L"[Packages]\r\n"
        L"; Package order is explicit. Empty entries are ignored.\r\n"
        L"; Relative paths are resolved from the ASI directory.\r\n"
        L"Package1=XBOX.tpf\r\n"
        L"Package2=\r\n"
        L"Package3=\r\n";

    HANDLE file = CreateFileW(g_iniPath, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                              CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    const unsigned char bom[2] = {0xFF, 0xFE};
    DWORD written = 0;
    WriteFile(file, bom, 2, &written, nullptr);
    WriteFile(file, text, (DWORD)(wcslen(text) * sizeof(wchar_t)), &written, nullptr);
    CloseHandle(file);
}

static int ReadIniInt(const wchar_t* section, const wchar_t* key, int defaultValue)
{
    return GetPrivateProfileIntW(section, key, defaultValue, g_iniPath);
}

static std::wstring ReadIniString(const wchar_t* section, const wchar_t* key, const wchar_t* defaultValue = L"")
{
    wchar_t buffer[MAX_PATH * 4] = {};
    GetPrivateProfileStringW(section, key, defaultValue, buffer, _countof(buffer), g_iniPath);
    return buffer;
}

static void ReadConfiguration()
{
    g_enabled = ReadIniInt(L"TPFLoader", L"Enable", 1) != 0;
    g_logLevel = std::max(0, std::min(3, ReadIniInt(L"TPFLoader", L"LogLevel", 2)));
    g_laterPackageWins = ReadIniInt(L"TPFLoader", L"LaterPackageWins", 1) != 0;
    g_onlyTargetDimensions = ReadIniInt(L"TPFLoader", L"OnlyTargetDimensions", 1) != 0;
    g_maxHashDimension = std::max(0, std::min(16384, ReadIniInt(L"TPFLoader", L"MaxHashDimension", 512)));
    g_logUnmatched = ReadIniInt(L"TPFLoader", L"LogUnmatched", 0) != 0;
    g_maxUnmatchedLogs = std::max(0, std::min(100000, ReadIniInt(L"TPFLoader", L"MaxUnmatchedLogs", 80)));
}

// ---------------------------- Package loading ------------------------------
static uint64_t DimensionKey(uint32_t width, uint32_t height)
{
    return ((uint64_t)width << 32) | height;
}

static void LoadConfiguredPackages()
{
    for (int i = 1; i <= 64; ++i) {
        wchar_t key[32] = {};
        wsprintfW(key, L"Package%d", i);
        std::wstring configured = ReadIniString(L"Packages", key, L"");
        if (configured.empty()) continue;

        std::wstring path = ResolvePath(configured.c_str());
        TpfLoadResult loaded = LoadTpfOrZipPackage(path);
        char pathUtf8[1200] = {};
        WideToUtf8(path.c_str(), pathUtf8, sizeof(pathUtf8));

        if (!loaded.ok) {
            char errorUtf8[1200] = {};
            WideToUtf8(loaded.error.c_str(), errorUtf8, sizeof(errorUtf8));
            Log(1, "Package%d failed: %s; %s", i, pathUtf8, errorUtf8);
            continue;
        }

        ++g_loadedPackageCount;
        int uniqueInPackage = 0;
        std::set<uint32_t> seenInPackage;
        for (TpfTextureEntry& tex : loaded.textures) {
            if (!seenInPackage.insert(tex.hash).second) ++g_duplicateDefinitionCount;
            else ++uniqueInPackage;

            ReplacementEntry replacement;
            replacement.hash = tex.hash;
            replacement.packagePath = path;
            replacement.internalName = tex.internalName;
            replacement.imageData.swap(tex.imageData);
            replacement.imageWidth = tex.imageWidth;
            replacement.imageHeight = tex.imageHeight;
            replacement.packageOrder = i;

            auto existing = g_replacements.find(replacement.hash);
            if (existing == g_replacements.end() || g_laterPackageWins)
                g_replacements[replacement.hash] = std::move(replacement);

            if (tex.imageWidth && tex.imageHeight)
                g_targetDimensions.insert(DimensionKey(tex.imageWidth, tex.imageHeight));
        }

        Log(2, "Package%d loaded: %s; definitions=%u unique-in-package=%d",
            i, pathUtf8, (unsigned)loaded.textures.size(), uniqueInPackage);
    }

    Log(2, "Package database ready: packages=%d unique-hashes=%u duplicate-definitions=%d target-dimensions=%u",
        g_loadedPackageCount, (unsigned)g_replacements.size(), g_duplicateDefinitionCount,
        (unsigned)g_targetDimensions.size());
}

// ----------------------------- D3DX resolving -------------------------------
static void ResolveD3DX()
{
    const wchar_t* names[] = {
        L"d3dx9_43.dll", L"d3dx9_42.dll", L"d3dx9_41.dll", L"d3dx9_40.dll",
        L"d3dx9_39.dll", L"d3dx9_38.dll", L"d3dx9_37.dll", L"d3dx9_36.dll",
        L"d3dx9_35.dll", L"d3dx9_34.dll", L"d3dx9_33.dll", L"d3dx9_32.dll",
        L"d3dx9_31.dll", L"d3dx9_30.dll", L"d3dx9_29.dll", L"d3dx9_28.dll",
        L"d3dx9_27.dll", L"d3dx9_26.dll", L"d3dx9_25.dll", L"d3dx9_24.dll"
    };
    for (const wchar_t* name : names) {
        HMODULE module = GetModuleHandleW(name);
        if (!module) module = LoadLibraryW(name);
        if (!module) continue;
        FARPROC p = GetProcAddress(module, "D3DXCreateTextureFromFileInMemoryEx");
        if (p) {
            g_d3dxModule = module;
            g_D3DXCreateTextureFromFileInMemoryEx = (D3DXCreateTextureFromFileInMemoryEx_t)p;
            char nameUtf8[128] = {};
            WideToUtf8(name, nameUtf8, sizeof(nameUtf8));
            Log(2, "D3DX image loader resolved from %s", nameUtf8);
            return;
        }
    }
    Log(1, "D3DXCreateTextureFromFileInMemoryEx was not found; packages can be parsed but matched images cannot be created");
}

// ---------------------------- Vtable patching -------------------------------
static bool PatchVtable(void** vtable, int index, void* hook, void** original, const char* label)
{
    if (!vtable || !hook || !original) return false;
    void** slot = &vtable[index];
    void* current = *slot;
    if (current == hook) return true;
    if (*original && current != *original) {
        Log(1, "%s vtable slot is already replaced by another module; hook skipped", label);
        return false;
    }

    DWORD oldProtect = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        Log(1, "%s VirtualProtect failed, error=%lu", label, GetLastError());
        return false;
    }
    if (!*original) *original = current;
    *slot = hook;
    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(void*), oldProtect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));
    return true;
}

static bool PatchMainModuleIAT(const char* dllName, const char* procName, void* hook, void** original)
{
    HMODULE module = GetModuleHandleW(nullptr);
    if (!module) return false;
    unsigned char* base = (unsigned char*)module;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    DWORD importRva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (!importRva) return false;

    IMAGE_IMPORT_DESCRIPTOR* descriptor = (IMAGE_IMPORT_DESCRIPTOR*)(base + importRva);
    for (; descriptor->Name; ++descriptor) {
        const char* importedDll = (const char*)(base + descriptor->Name);
        if (_stricmp(importedDll, dllName) != 0) continue;

        IMAGE_THUNK_DATA* names = descriptor->OriginalFirstThunk
            ? (IMAGE_THUNK_DATA*)(base + descriptor->OriginalFirstThunk)
            : (IMAGE_THUNK_DATA*)(base + descriptor->FirstThunk);
        IMAGE_THUNK_DATA* iat = (IMAGE_THUNK_DATA*)(base + descriptor->FirstThunk);
        for (int i = 0; names[i].u1.AddressOfData; ++i) {
            if (IMAGE_SNAP_BY_ORDINAL(names[i].u1.Ordinal)) continue;
            IMAGE_IMPORT_BY_NAME* name = (IMAGE_IMPORT_BY_NAME*)(base + names[i].u1.AddressOfData);
            if (strcmp((const char*)name->Name, procName) != 0) continue;

            void** slot = (void**)&iat[i].u1.Function;
            DWORD oldProtect = 0;
            if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtect)) return false;
            *original = *slot;
            *slot = hook;
            DWORD ignored = 0;
            VirtualProtect(slot, sizeof(void*), oldProtect, &ignored);
            FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));
            return true;
        }
    }
    return false;
}

// ------------------------------ Hashing -------------------------------------
struct HashCandidate {
    uint32_t value = 0;
    const char* mode = nullptr;
};

static uint32_t ByteSwap32(uint32_t v)
{
    return ((v & 0x000000FFu) << 24) |
           ((v & 0x0000FF00u) << 8) |
           ((v & 0x00FF0000u) >> 8) |
           ((v & 0xFF000000u) >> 24);
}

static void AddCandidateVariants(std::vector<HashCandidate>& out, uint32_t crc, const char* mode)
{
    out.push_back({crc, mode});

    static char complementNames[4][64];
    static char swapNames[4][64];
    static char swapComplementNames[4][64];
    // Names are only diagnostic. Build stable strings for the four base modes.
    int slot = 0;
    if (strstr(mode, "pitch")) slot += 1;
    if (strstr(mode, "all")) slot += 2;
#if defined(_MSC_VER)
    _snprintf_s(complementNames[slot], sizeof(complementNames[slot]), _TRUNCATE, "%s-complement", mode);
    _snprintf_s(swapNames[slot], sizeof(swapNames[slot]), _TRUNCATE, "%s-byteswap", mode);
    _snprintf_s(swapComplementNames[slot], sizeof(swapComplementNames[slot]), _TRUNCATE, "%s-complement-byteswap", mode);
#else
    snprintf(complementNames[slot], sizeof(complementNames[slot]), "%s-complement", mode);
    snprintf(swapNames[slot], sizeof(swapNames[slot]), "%s-byteswap", mode);
    snprintf(swapComplementNames[slot], sizeof(swapComplementNames[slot]), "%s-complement-byteswap", mode);
#endif
    out.push_back({~crc, complementNames[slot]});
    out.push_back({ByteSwap32(crc), swapNames[slot]});
    out.push_back({ByteSwap32(~crc), swapComplementNames[slot]});
}

static bool FormatLayout(D3DFORMAT format, UINT width, UINT height, UINT pitch,
                         UINT& rows, UINT& compactRowBytes)
{
    switch (format) {
    case D3DFMT_DXT1:
        rows = std::max<UINT>(1, (height + 3) / 4);
        compactRowBytes = std::max<UINT>(1, (width + 3) / 4) * 8;
        return true;
    case D3DFMT_DXT2:
    case D3DFMT_DXT3:
    case D3DFMT_DXT4:
    case D3DFMT_DXT5:
        rows = std::max<UINT>(1, (height + 3) / 4);
        compactRowBytes = std::max<UINT>(1, (width + 3) / 4) * 16;
        return true;
    default:
        break;
    }

    UINT bpp = 0;
    switch (format) {
    case D3DFMT_R8G8B8: bpp = 3; break;
    case D3DFMT_A8R8G8B8:
    case D3DFMT_X8R8G8B8:
    case D3DFMT_A8B8G8R8:
    case D3DFMT_X8B8G8R8:
    case D3DFMT_G16R16:
    case D3DFMT_A2R10G10B10:
    case D3DFMT_A2B10G10R10:
    case D3DFMT_Q8W8V8U8:
    case D3DFMT_V16U16:
    case D3DFMT_X8L8V8U8:
        bpp = 4; break;
    case D3DFMT_R5G6B5:
    case D3DFMT_X1R5G5B5:
    case D3DFMT_A1R5G5B5:
    case D3DFMT_A4R4G4B4:
    case D3DFMT_A8R3G3B2:
    case D3DFMT_X4R4G4B4:
    case D3DFMT_A8L8:
    case D3DFMT_V8U8:
    case D3DFMT_L6V5U5:
    case D3DFMT_CxV8U8:
        bpp = 2; break;
    case D3DFMT_A8:
    case D3DFMT_L8:
    case D3DFMT_P8:
    case D3DFMT_A4L4:
        bpp = 1; break;
    default:
        rows = height;
        compactRowBytes = pitch;
        return false;
    }
    rows = height;
    compactRowBytes = width * bpp;
    return true;
}

static uint32_t UpdateCrcRows(uint32_t crc, const D3DLOCKED_RECT& locked,
                              UINT rows, UINT rowBytes)
{
    const unsigned char* row = (const unsigned char*)locked.pBits;
    UINT pitch = (UINT)(locked.Pitch < 0 ? -locked.Pitch : locked.Pitch);
    rowBytes = std::min(rowBytes, pitch);
    for (UINT y = 0; y < rows; ++y) {
        crc = (uint32_t)crc32(crc, row, rowBytes);
        row += locked.Pitch;
    }
    return crc;
}

static bool ComputeTextureHashes(IDirect3DTexture9* texture,
                                 std::vector<HashCandidate>& candidates,
                                 D3DSURFACE_DESC& topDesc)
{
    if (!texture) return false;
    if (FAILED(texture->GetLevelDesc(0, &topDesc))) return false;

    if (g_maxHashDimension > 0 &&
        ((int)topDesc.Width > g_maxHashDimension || (int)topDesc.Height > g_maxHashDimension))
        return false;
    if (g_onlyTargetDimensions && !g_targetDimensions.empty() &&
        g_targetDimensions.find(DimensionKey(topDesc.Width, topDesc.Height)) == g_targetDimensions.end())
        return false;

    uint32_t topCompact = (uint32_t)crc32(0L, Z_NULL, 0);
    uint32_t topPitch = (uint32_t)crc32(0L, Z_NULL, 0);
    uint32_t allCompact = (uint32_t)crc32(0L, Z_NULL, 0);
    uint32_t allPitch = (uint32_t)crc32(0L, Z_NULL, 0);
    bool gotAny = false;

    UINT levels = texture->GetLevelCount();
    for (UINT level = 0; level < levels; ++level) {
        D3DSURFACE_DESC desc = {};
        if (FAILED(texture->GetLevelDesc(level, &desc))) break;
        D3DLOCKED_RECT locked = {};
        HRESULT hr = texture->LockRect(level, &locked, nullptr, D3DLOCK_READONLY);
        if (FAILED(hr) || !locked.pBits || locked.Pitch == 0) {
            if (level == 0) return false;
            break;
        }

        UINT pitch = (UINT)(locked.Pitch < 0 ? -locked.Pitch : locked.Pitch);
        UINT rows = desc.Height;
        UINT compactBytes = pitch;
        FormatLayout(desc.Format, desc.Width, desc.Height, pitch, rows, compactBytes);

        uint32_t compactLevel = (uint32_t)crc32(0L, Z_NULL, 0);
        uint32_t pitchLevel = (uint32_t)crc32(0L, Z_NULL, 0);
        compactLevel = UpdateCrcRows(compactLevel, locked, rows, compactBytes);
        pitchLevel = UpdateCrcRows(pitchLevel, locked, rows, pitch);
        allCompact = UpdateCrcRows(allCompact, locked, rows, compactBytes);
        allPitch = UpdateCrcRows(allPitch, locked, rows, pitch);

        texture->UnlockRect(level);
        if (level == 0) {
            topCompact = compactLevel;
            topPitch = pitchLevel;
        }
        gotAny = true;
    }

    if (!gotAny) return false;
    AddCandidateVariants(candidates, topCompact, "top-compact");
    AddCandidateVariants(candidates, topPitch, "top-pitch");
    AddCandidateVariants(candidates, allCompact, "all-compact");
    AddCandidateVariants(candidates, allPitch, "all-pitch");
    return true;
}

static const ReplacementEntry* FindReplacement(const std::vector<HashCandidate>& candidates,
                                               const HashCandidate** matchedCandidate)
{
    for (const HashCandidate& candidate : candidates) {
        auto it = g_replacements.find(candidate.value);
        if (it != g_replacements.end()) {
            if (matchedCandidate) *matchedCandidate = &candidate;
            return &it->second;
        }
    }
    return nullptr;
}

static IDirect3DTexture9* CreateReplacementTexture(IDirect3DDevice9* device,
                                                   const ReplacementEntry& replacement,
                                                   const D3DSURFACE_DESC& sourceDesc)
{
    if (!device || !g_D3DXCreateTextureFromFileInMemoryEx || replacement.imageData.empty()) return nullptr;

    IDirect3DTexture9* texture = nullptr;
    InterlockedIncrement(&g_internalTextureCreate);
    HRESULT hr = g_D3DXCreateTextureFromFileInMemoryEx(
        device,
        replacement.imageData.data(),
        (UINT)replacement.imageData.size(),
        sourceDesc.Width,
        sourceDesc.Height,
        D3DX_DEFAULT_LOCAL,
        0,
        D3DFMT_UNKNOWN,
        D3DPOOL_MANAGED,
        D3DX_FILTER_DEFAULT_LOCAL,
        D3DX_FILTER_DEFAULT_LOCAL,
        0,
        nullptr,
        nullptr,
        &texture);
    InterlockedDecrement(&g_internalTextureCreate);

    if (FAILED(hr)) {
        Log(1, "D3DX replacement creation failed for hash=0x%08X, HRESULT=0x%08lX",
            replacement.hash, (unsigned long)hr);
        return nullptr;
    }
    return texture;
}

static void MarkTextureDirty(IDirect3DTexture9* texture)
{
    if (!texture || !g_textureLockReady) return;
    EnterCriticalSection(&g_textureLock);
    auto it = g_runtimeTextures.find(texture);
    if (it != g_runtimeTextures.end()) {
        it->second.dirty = true;
        it->second.attempted = false;
        if (it->second.replacement) {
            IDirect3DTexture9* old = it->second.replacement;
            it->second.replacement = nullptr;
            it->second.matchedHash = 0;
            LeaveCriticalSection(&g_textureLock);
            old->Release();
            return;
        }
    }
    LeaveCriticalSection(&g_textureLock);
}

static IDirect3DTexture9* ResolveRuntimeReplacement(IDirect3DDevice9* device,
                                                   IDirect3DTexture9* original)
{
    if (!original || !g_textureLockReady) return nullptr;

    EnterCriticalSection(&g_textureLock);
    auto found = g_runtimeTextures.find(original);
    if (found == g_runtimeTextures.end()) {
        LeaveCriticalSection(&g_textureLock);
        return nullptr;
    }
    if (found->second.replacement && !found->second.dirty) {
        IDirect3DTexture9* ready = found->second.replacement;
        LeaveCriticalSection(&g_textureLock);
        return ready;
    }
    if (found->second.attempted && !found->second.dirty) {
        LeaveCriticalSection(&g_textureLock);
        return nullptr;
    }
    LeaveCriticalSection(&g_textureLock);

    std::vector<HashCandidate> candidates;
    D3DSURFACE_DESC desc = {};
    if (!ComputeTextureHashes(original, candidates, desc)) {
        EnterCriticalSection(&g_textureLock);
        auto it = g_runtimeTextures.find(original);
        if (it != g_runtimeTextures.end()) {
            it->second.attempted = true;
            it->second.dirty = false;
        }
        LeaveCriticalSection(&g_textureLock);
        return nullptr;
    }

    const HashCandidate* matchedMode = nullptr;
    const ReplacementEntry* replacement = FindReplacement(candidates, &matchedMode);
    if (!replacement) {
        if (g_logLevel >= 3 && g_logUnmatched &&
            InterlockedIncrement(&g_unmatchedLogCount) <= g_maxUnmatchedLogs) {
            Log(3, "Unmatched texture %ux%u fmt=%u: topCompact=0x%08X topPitch=0x%08X allCompact=0x%08X allPitch=0x%08X",
                desc.Width, desc.Height, (unsigned)desc.Format,
                candidates.size() > 0 ? candidates[0].value : 0,
                candidates.size() > 4 ? candidates[4].value : 0,
                candidates.size() > 8 ? candidates[8].value : 0,
                candidates.size() > 12 ? candidates[12].value : 0);
        }
        EnterCriticalSection(&g_textureLock);
        auto it = g_runtimeTextures.find(original);
        if (it != g_runtimeTextures.end()) {
            it->second.attempted = true;
            it->second.dirty = false;
        }
        LeaveCriticalSection(&g_textureLock);
        return nullptr;
    }

    IDirect3DTexture9* created = CreateReplacementTexture(device, *replacement, desc);
    if (!created) return nullptr;

    EnterCriticalSection(&g_textureLock);
    auto it = g_runtimeTextures.find(original);
    if (it == g_runtimeTextures.end()) {
        LeaveCriticalSection(&g_textureLock);
        created->Release();
        return nullptr;
    }
    if (it->second.replacement) it->second.replacement->Release();
    it->second.replacement = created;
    it->second.matchedHash = replacement->hash;
    it->second.dirty = false;
    it->second.attempted = true;
    LeaveCriticalSection(&g_textureLock);

    char packageUtf8[1200] = {};
    WideToUtf8(replacement->packagePath.c_str(), packageUtf8, sizeof(packageUtf8));
    Log(2, "Texture replaced: hash=0x%08X mode=%s original=%ux%u package=%s entry=%s",
        replacement->hash, matchedMode ? matchedMode->mode : "unknown",
        desc.Width, desc.Height, packageUtf8, replacement->internalName.c_str());
    return created;
}

static void ClearAllReplacementTextures()
{
    std::vector<IDirect3DTexture9*> releaseList;
    EnterCriticalSection(&g_textureLock);
    for (auto& pair : g_runtimeTextures) {
        if (pair.second.replacement) {
            releaseList.push_back(pair.second.replacement);
            pair.second.replacement = nullptr;
        }
        pair.second.matchedHash = 0;
        pair.second.dirty = true;
        pair.second.attempted = false;
    }
    LeaveCriticalSection(&g_textureLock);
    for (IDirect3DTexture9* texture : releaseList) texture->Release();
}

// ------------------------------- Hooks -------------------------------------
static ULONG STDMETHODCALLTYPE Hook_Texture_Release(IDirect3DTexture9* self)
{
    ULONG count = Real_Texture_Release ? Real_Texture_Release(self) : 0;
    if (count == 0 && g_textureLockReady) {
        IDirect3DTexture9* replacement = nullptr;
        EnterCriticalSection(&g_textureLock);
        auto it = g_runtimeTextures.find(self);
        if (it != g_runtimeTextures.end()) {
            replacement = it->second.replacement;
            g_runtimeTextures.erase(it);
        }
        LeaveCriticalSection(&g_textureLock);
        if (replacement) replacement->Release();
    }
    return count;
}

static HRESULT STDMETHODCALLTYPE Hook_Texture_LockRect(IDirect3DTexture9* self, UINT level,
                                                       D3DLOCKED_RECT* locked, const RECT* rect, DWORD flags)
{
    return Real_Texture_LockRect ? Real_Texture_LockRect(self, level, locked, rect, flags) : D3DERR_INVALIDCALL;
}

static HRESULT STDMETHODCALLTYPE Hook_Texture_UnlockRect(IDirect3DTexture9* self, UINT level)
{
    HRESULT hr = Real_Texture_UnlockRect ? Real_Texture_UnlockRect(self, level) : D3DERR_INVALIDCALL;
    if (SUCCEEDED(hr)) MarkTextureDirty(self);
    return hr;
}

static HRESULT STDMETHODCALLTYPE Hook_Texture_AddDirtyRect(IDirect3DTexture9* self, const RECT* rect)
{
    HRESULT hr = Real_Texture_AddDirtyRect ? Real_Texture_AddDirtyRect(self, rect) : D3DERR_INVALIDCALL;
    if (SUCCEEDED(hr)) MarkTextureDirty(self);
    return hr;
}

static void PatchTextureObject(IDirect3DTexture9* texture)
{
    if (!texture) return;
    void** vtable = *(void***)texture;
    PatchVtable(vtable, VTBL_TEXTURE_RELEASE, (void*)Hook_Texture_Release,
                (void**)&Real_Texture_Release, "IDirect3DTexture9::Release");
    PatchVtable(vtable, VTBL_TEXTURE_LOCK_RECT, (void*)Hook_Texture_LockRect,
                (void**)&Real_Texture_LockRect, "IDirect3DTexture9::LockRect");
    PatchVtable(vtable, VTBL_TEXTURE_UNLOCK_RECT, (void*)Hook_Texture_UnlockRect,
                (void**)&Real_Texture_UnlockRect, "IDirect3DTexture9::UnlockRect");
    PatchVtable(vtable, VTBL_TEXTURE_ADD_DIRTY_RECT, (void*)Hook_Texture_AddDirtyRect,
                (void**)&Real_Texture_AddDirtyRect, "IDirect3DTexture9::AddDirtyRect");
}

static HRESULT STDMETHODCALLTYPE Hook_Device_CreateTexture(IDirect3DDevice9* self,
    UINT width, UINT height, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool,
    IDirect3DTexture9** output, HANDLE* sharedHandle)
{
    HRESULT hr = Real_Device_CreateTexture
        ? Real_Device_CreateTexture(self, width, height, levels, usage, format, pool, output, sharedHandle)
        : D3DERR_INVALIDCALL;
    if (SUCCEEDED(hr) && output && *output) {
        PatchTextureObject(*output);
        if (InterlockedCompareExchange(&g_internalTextureCreate, 0, 0) == 0) {
            EnterCriticalSection(&g_textureLock);
            g_runtimeTextures.emplace(*output, RuntimeTexture());
            LeaveCriticalSection(&g_textureLock);
        }
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE Hook_Device_UpdateTexture(IDirect3DDevice9* self,
                                                            IDirect3DBaseTexture9* source,
                                                            IDirect3DBaseTexture9* destination)
{
    HRESULT hr = Real_Device_UpdateTexture
        ? Real_Device_UpdateTexture(self, source, destination)
        : D3DERR_INVALIDCALL;
    if (SUCCEEDED(hr) && destination && destination->GetType() == D3DRTYPE_TEXTURE)
        MarkTextureDirty((IDirect3DTexture9*)destination);
    return hr;
}

static HRESULT STDMETHODCALLTYPE Hook_Device_SetTexture(IDirect3DDevice9* self,
                                                         DWORD stage,
                                                         IDirect3DBaseTexture9* texture)
{
    IDirect3DBaseTexture9* submitted = texture;
    if (texture && texture->GetType() == D3DRTYPE_TEXTURE) {
        IDirect3DTexture9* replacement = ResolveRuntimeReplacement(self, (IDirect3DTexture9*)texture);
        if (replacement) submitted = replacement;
    }
    return Real_Device_SetTexture ? Real_Device_SetTexture(self, stage, submitted) : D3DERR_INVALIDCALL;
}

static HRESULT STDMETHODCALLTYPE Hook_Device_Reset(IDirect3DDevice9* self,
                                                    D3DPRESENT_PARAMETERS* parameters)
{
    ClearAllReplacementTextures();
    HRESULT hr = Real_Device_Reset ? Real_Device_Reset(self, parameters) : D3DERR_INVALIDCALL;
    Log(SUCCEEDED(hr) ? 2 : 1, "IDirect3DDevice9::Reset returned HRESULT=0x%08lX", (unsigned long)hr);
    return hr;
}

static ULONG STDMETHODCALLTYPE Hook_Device_Release(IDirect3DDevice9* self)
{
    ULONG count = Real_Device_Release ? Real_Device_Release(self) : 0;
    if (count == 0 && self == g_lastDevice) {
        g_lastDevice = nullptr;
        Log(2, "Direct3D device released");
    }
    return count;
}

static void PatchDeviceObject(IDirect3DDevice9* device)
{
    if (!device) return;
    void** vtable = *(void***)device;
    bool ok = true;
    ok &= PatchVtable(vtable, VTBL_DEVICE_RELEASE, (void*)Hook_Device_Release,
                      (void**)&Real_Device_Release, "IDirect3DDevice9::Release");
    ok &= PatchVtable(vtable, VTBL_DEVICE_RESET, (void*)Hook_Device_Reset,
                      (void**)&Real_Device_Reset, "IDirect3DDevice9::Reset");
    ok &= PatchVtable(vtable, VTBL_DEVICE_CREATE_TEXTURE, (void*)Hook_Device_CreateTexture,
                      (void**)&Real_Device_CreateTexture, "IDirect3DDevice9::CreateTexture");
    ok &= PatchVtable(vtable, VTBL_DEVICE_UPDATE_TEXTURE, (void*)Hook_Device_UpdateTexture,
                      (void**)&Real_Device_UpdateTexture, "IDirect3DDevice9::UpdateTexture");
    ok &= PatchVtable(vtable, VTBL_DEVICE_SET_TEXTURE, (void*)Hook_Device_SetTexture,
                      (void**)&Real_Device_SetTexture, "IDirect3DDevice9::SetTexture");
    if (ok) {
        g_lastDevice = device;
        Log(2, "Direct3D 9 device hooks installed");
    }
}

static HRESULT STDMETHODCALLTYPE Hook_D3D9_CreateDevice(IDirect3D9* self,
    UINT adapter, D3DDEVTYPE type, HWND focusWindow, DWORD behavior,
    D3DPRESENT_PARAMETERS* parameters, IDirect3DDevice9** output)
{
    HRESULT hr = Real_D3D9_CreateDevice
        ? Real_D3D9_CreateDevice(self, adapter, type, focusWindow, behavior, parameters, output)
        : D3DERR_INVALIDCALL;
    if (SUCCEEDED(hr) && output && *output) PatchDeviceObject(*output);
    return hr;
}

static IDirect3D9* WINAPI Hook_Direct3DCreate9(UINT sdkVersion)
{
    IDirect3D9* d3d = Real_Direct3DCreate9 ? Real_Direct3DCreate9(sdkVersion) : nullptr;
    if (d3d) {
        void** vtable = *(void***)d3d;
        if (PatchVtable(vtable, VTBL_D3D9_CREATE_DEVICE, (void*)Hook_D3D9_CreateDevice,
                        (void**)&Real_D3D9_CreateDevice, "IDirect3D9::CreateDevice"))
            Log(2, "IDirect3D9::CreateDevice hook installed");
    }
    return d3d;
}

// ------------------------------- Startup -----------------------------------
static DWORD WINAPI InitializeThread(LPVOID)
{
    BuildPaths();
    CreateDefaultIniIfMissing();
    ReadConfiguration();

    DeleteFileW(g_logPath);
    if (!g_enabled) {
        Log(1, "TPF loader disabled by configuration");
        return 0;
    }

    InitializeCriticalSection(&g_textureLock);
    g_textureLockReady = true;

    char iniUtf8[1200] = {};
    WideToUtf8(g_iniPath, iniUtf8, sizeof(iniUtf8));
    Log(2, "loaded (experimental standalone build)");
    Log(2, "ini=%s", iniUtf8);
    Log(2, "configuration: laterWins=%d onlyTargetDimensions=%d maxHashDimension=%d logUnmatched=%d",
        g_laterPackageWins ? 1 : 0, g_onlyTargetDimensions ? 1 : 0,
        g_maxHashDimension, g_logUnmatched ? 1 : 0);

    LoadConfiguredPackages();
    if (g_replacements.empty()) {
        Log(1, "No usable replacement textures were loaded; D3D hooks will not be installed");
        return 0;
    }

    ResolveD3DX();
    if (!PatchMainModuleIAT("d3d9.dll", "Direct3DCreate9",
                            (void*)Hook_Direct3DCreate9,
                            (void**)&Real_Direct3DCreate9)) {
        Log(1, "Direct3DCreate9 IAT hook was not installed; the game may use another D3D9 loading path");
        return 0;
    }

    Log(2, "Direct3DCreate9 IAT hook installed; waiting for the game to create its D3D9 device");
    return 0;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = module;
        DisableThreadLibraryCalls(module);
        HANDLE thread = CreateThread(nullptr, 0, InitializeThread, nullptr, 0, nullptr);
        if (thread) CloseHandle(thread);
    }
    return TRUE;
}
