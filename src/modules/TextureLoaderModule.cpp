#include "TextureLoaderModule.h"
#include "../texture/TpfArchive.h"

#include <d3d9.h>
#include <zlib.h>

namespace popopt::textureloader {

struct ReplacementEntry {
    uint32_t hash = 0;
    std::wstring packagePath;
    std::string internalName;
    std::vector<unsigned char> imageData;
    int packageOrder = 0;
};

struct RuntimeTexture {
    IDirect3DTexture9* replacement = nullptr;
    uint32_t matchedHash = 0;
    bool dirty = true;
    bool attempted = false;
};

static bool g_enabled = false;
static bool g_laterPackageWins = true;
static bool g_logUnmatched = false;
static int g_maxUnmatchedLogs = 256;
static int g_loadedPackages = 0;
static int g_duplicateDefinitions = 0;
static volatile LONG g_replacedTextures = 0;
static volatile LONG g_unmatchedLogs = 0;
static volatile LONG g_internalTextureCreate = 0;
static bool g_d3dxReady = false;
static bool g_d3dIatHookInstalled = false;

static std::unordered_map<uint32_t, ReplacementEntry> g_replacements;
static std::unordered_map<IDirect3DTexture9*, RuntimeTexture> g_runtimeTextures;
static CRITICAL_SECTION g_textureLock;
static bool g_textureLockReady = false;
static IDirect3DDevice9* g_lastDevice = nullptr;

#define D3DX_DEFAULT_LOCAL 0xFFFFFFFFu
#define D3DX_FILTER_DEFAULT_LOCAL 0xFFFFFFFFu

using D3DXCreateTextureFromFileInMemoryEx_t = HRESULT (WINAPI *)(
    IDirect3DDevice9*, LPCVOID, UINT, UINT, UINT, UINT, DWORD,
    D3DFORMAT, D3DPOOL, DWORD, DWORD, D3DCOLOR, void*, PALETTEENTRY*,
    IDirect3DTexture9**);

static HMODULE g_d3dxModule = nullptr;
static D3DXCreateTextureFromFileInMemoryEx_t g_createTextureFromMemory = nullptr;

using Direct3DCreate9_t = IDirect3D9* (WINAPI *)(UINT);
using D3D9_CreateDevice_t = HRESULT (STDMETHODCALLTYPE *)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
using Device_Release_t = ULONG (STDMETHODCALLTYPE *)(IDirect3DDevice9*);
using Device_Reset_t = HRESULT (STDMETHODCALLTYPE *)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
using Device_CreateTexture_t = HRESULT (STDMETHODCALLTYPE *)(IDirect3DDevice9*, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DTexture9**, HANDLE*);
using Device_UpdateTexture_t = HRESULT (STDMETHODCALLTYPE *)(IDirect3DDevice9*, IDirect3DBaseTexture9*, IDirect3DBaseTexture9*);
using Device_SetTexture_t = HRESULT (STDMETHODCALLTYPE *)(IDirect3DDevice9*, DWORD, IDirect3DBaseTexture9*);
using Texture_Release_t = ULONG (STDMETHODCALLTYPE *)(IDirect3DTexture9*);
using Texture_LockRect_t = HRESULT (STDMETHODCALLTYPE *)(IDirect3DTexture9*, UINT, D3DLOCKED_RECT*, const RECT*, DWORD);
using Texture_UnlockRect_t = HRESULT (STDMETHODCALLTYPE *)(IDirect3DTexture9*, UINT);
using Texture_AddDirtyRect_t = HRESULT (STDMETHODCALLTYPE *)(IDirect3DTexture9*, const RECT*);

static Direct3DCreate9_t g_realDirect3DCreate9 = nullptr;
static D3D9_CreateDevice_t g_realCreateDevice = nullptr;
static Device_Release_t g_realDeviceRelease = nullptr;
static Device_Reset_t g_realReset = nullptr;
static Device_CreateTexture_t g_realCreateTexture = nullptr;
static Device_UpdateTexture_t g_realUpdateTexture = nullptr;
static Device_SetTexture_t g_realSetTexture = nullptr;
static Texture_Release_t g_realTextureRelease = nullptr;
static Texture_LockRect_t g_realLockRect = nullptr;
static Texture_UnlockRect_t g_realUnlockRect = nullptr;
static Texture_AddDirtyRect_t g_realAddDirtyRect = nullptr;

static constexpr int kVtableD3D9CreateDevice = 16;
static constexpr int kVtableDeviceRelease = 2;
static constexpr int kVtableDeviceReset = 16;
static constexpr int kVtableDeviceCreateTexture = 23;
static constexpr int kVtableDeviceUpdateTexture = 31;
static constexpr int kVtableDeviceSetTexture = 65;
static constexpr int kVtableTextureRelease = 2;
static constexpr int kVtableTextureLockRect = 19;
static constexpr int kVtableTextureUnlockRect = 20;
static constexpr int kVtableTextureAddDirtyRect = 21;

static std::wstring ResolvePackagePath(const std::wstring& configured)
{
    if (configured.empty()) return {};
    if ((configured.size() >= 2 && configured[1] == L':') ||
        (configured.size() >= 2 && configured[0] == L'\\' && configured[1] == L'\\'))
        return configured;
    std::wstring result = App().asiDir;
    if (!result.empty() && result.back() != L'\\') result += L'\\';
    result += configured;
    return result;
}

static void LoadPackages()
{
    for (int index = 1; index <= 64; ++index) {
        wchar_t key[32] = {};
        wsprintfW(key, L"Package%d", index);
        std::wstring configured = Ini().GetString(L"TexturePackages", key, L"");
        if (configured.empty()) continue;

        std::wstring path = ResolvePackagePath(configured);
        TpfLoadResult loaded = LoadTpfOrZipPackage(path);
        char pathUtf8[kPathCapacity] = {};
        WideToUtf8(path.c_str(), pathUtf8, (int)sizeof(pathUtf8));
        if (!loaded.ok) {
            char errorUtf8[kPathCapacity] = {};
            WideToUtf8(loaded.error.c_str(), errorUtf8, (int)sizeof(errorUtf8));
            Log().Write(1, "Texture Package%d failed: %s; %s", index, pathUtf8, errorUtf8);
            continue;
        }

        ++g_loadedPackages;
        std::set<uint32_t> seen;
        for (TpfTextureEntry& texture : loaded.textures) {
            if (!seen.insert(texture.hash).second) ++g_duplicateDefinitions;

            ReplacementEntry replacement;
            replacement.hash = texture.hash;
            replacement.packagePath = path;
            replacement.internalName = texture.internalName;
            replacement.imageData.swap(texture.imageData);
            replacement.packageOrder = index;

            auto existing = g_replacements.find(replacement.hash);
            if (existing == g_replacements.end() || g_laterPackageWins)
                g_replacements[replacement.hash] = std::move(replacement);
        }
        Log().Write(2, "Texture Package%d loaded: %s; definitions=%u",
                    index, pathUtf8, (unsigned)loaded.textures.size());
    }
    Log().Write(2, "Texture package database ready: packages=%d unique-hashes=%u duplicate-definitions=%d",
                g_loadedPackages, (unsigned)g_replacements.size(), g_duplicateDefinitions);
}

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
        FARPROC proc = GetProcAddress(module, "D3DXCreateTextureFromFileInMemoryEx");
        if (!proc) continue;
        g_d3dxModule = module;
        g_createTextureFromMemory = (D3DXCreateTextureFromFileInMemoryEx_t)proc;
        g_d3dxReady = true;
        char utf8[128] = {};
        WideToUtf8(name, utf8, sizeof(utf8));
        Log().Write(2, "Texture image loader resolved from %s", utf8);
        return;
    }
    Log().Write(1, "D3DXCreateTextureFromFileInMemoryEx was not found; texture replacement disabled");
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
    default: break;
    }

    UINT bytesPerPixel = 0;
    switch (format) {
    case D3DFMT_R8G8B8: bytesPerPixel = 3; break;
    case D3DFMT_A8R8G8B8:
    case D3DFMT_X8R8G8B8:
    case D3DFMT_A8B8G8R8:
    case D3DFMT_X8B8G8R8:
    case D3DFMT_G16R16:
    case D3DFMT_A2R10G10B10:
    case D3DFMT_A2B10G10R10:
    case D3DFMT_Q8W8V8U8:
    case D3DFMT_V16U16:
    case D3DFMT_X8L8V8U8: bytesPerPixel = 4; break;
    case D3DFMT_R5G6B5:
    case D3DFMT_X1R5G5B5:
    case D3DFMT_A1R5G5B5:
    case D3DFMT_A4R4G4B4:
    case D3DFMT_A8R3G3B2:
    case D3DFMT_X4R4G4B4:
    case D3DFMT_A8L8:
    case D3DFMT_V8U8:
    case D3DFMT_L6V5U5:
    case D3DFMT_CxV8U8: bytesPerPixel = 2; break;
    case D3DFMT_A8:
    case D3DFMT_L8:
    case D3DFMT_P8:
    case D3DFMT_A4L4: bytesPerPixel = 1; break;
    default:
        rows = height;
        compactRowBytes = pitch;
        return false;
    }
    rows = height;
    compactRowBytes = width * bytesPerPixel;
    return true;
}

static uint32_t ComputeTexModHash(IDirect3DTexture9* texture, D3DSURFACE_DESC& description)
{
    if (!texture || FAILED(texture->GetLevelDesc(0, &description))) return 0;
    D3DLOCKED_RECT locked = {};
    if (FAILED(texture->LockRect(0, &locked, nullptr, D3DLOCK_READONLY)) ||
        !locked.pBits || locked.Pitch == 0)
        return 0;

    const UINT pitch = (UINT)(locked.Pitch < 0 ? -locked.Pitch : locked.Pitch);
    UINT rows = description.Height;
    UINT compactRowBytes = pitch;
    FormatLayout(description.Format, description.Width, description.Height,
                 pitch, rows, compactRowBytes);
    compactRowBytes = std::min(compactRowBytes, pitch);

    uint32_t crc = (uint32_t)crc32(0L, Z_NULL, 0);
    const unsigned char* row = (const unsigned char*)locked.pBits;
    for (UINT y = 0; y < rows; ++y) {
        crc = (uint32_t)crc32(crc, row, compactRowBytes);
        row += locked.Pitch;
    }
    texture->UnlockRect(0);
    return ~crc; // Verified in PoP as TexMod's top-compact-complement hash.
}

static IDirect3DTexture9* CreateReplacement(IDirect3DDevice9* device,
                                            const ReplacementEntry& replacement,
                                            const D3DSURFACE_DESC& source)
{
    if (!device || !g_createTextureFromMemory || replacement.imageData.empty()) return nullptr;
    IDirect3DTexture9* result = nullptr;
    InterlockedIncrement(&g_internalTextureCreate);
    HRESULT hr = g_createTextureFromMemory(
        device, replacement.imageData.data(), (UINT)replacement.imageData.size(),
        source.Width, source.Height, D3DX_DEFAULT_LOCAL, 0, D3DFMT_UNKNOWN,
        D3DPOOL_MANAGED, D3DX_FILTER_DEFAULT_LOCAL, D3DX_FILTER_DEFAULT_LOCAL,
        0, nullptr, nullptr, &result);
    InterlockedDecrement(&g_internalTextureCreate);
    if (FAILED(hr)) {
        Log().Write(1, "Texture replacement creation failed for hash=0x%08X, HRESULT=0x%08lX",
                    replacement.hash, (unsigned long)hr);
        return nullptr;
    }
    return result;
}

static void MarkDirty(IDirect3DTexture9* texture)
{
    if (!texture || !g_textureLockReady) return;
    IDirect3DTexture9* old = nullptr;
    EnterCriticalSection(&g_textureLock);
    auto it = g_runtimeTextures.find(texture);
    if (it != g_runtimeTextures.end()) {
        it->second.dirty = true;
        it->second.attempted = false;
        old = it->second.replacement;
        it->second.replacement = nullptr;
        it->second.matchedHash = 0;
    }
    LeaveCriticalSection(&g_textureLock);
    if (old) old->Release();
}

static IDirect3DTexture9* ResolveReplacement(IDirect3DDevice9* device,
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

    D3DSURFACE_DESC description = {};
    uint32_t hash = ComputeTexModHash(original, description);
    auto replacementIt = hash ? g_replacements.find(hash) : g_replacements.end();
    if (replacementIt == g_replacements.end()) {
        if (g_logUnmatched && Log().Level() >= 3 &&
            InterlockedIncrement(&g_unmatchedLogs) <= g_maxUnmatchedLogs) {
            Log().Write(3, "Unmatched texture %ux%u fmt=%u hash=0x%08X",
                        description.Width, description.Height,
                        (unsigned)description.Format, hash);
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

    IDirect3DTexture9* created = CreateReplacement(device, replacementIt->second, description);
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
    it->second.matchedHash = hash;
    it->second.dirty = false;
    it->second.attempted = true;
    LeaveCriticalSection(&g_textureLock);

    char packageUtf8[kPathCapacity] = {};
    WideToUtf8(replacementIt->second.packagePath.c_str(), packageUtf8, sizeof(packageUtf8));
    InterlockedIncrement(&g_replacedTextures);
    Log().Write(2, "Texture replaced: hash=0x%08X mode=top-compact-complement original=%ux%u package=%s entry=%s",
                hash, description.Width, description.Height, packageUtf8,
                replacementIt->second.internalName.c_str());
    return created;
}

static void ClearReplacements()
{
    if (!g_textureLockReady) return;
    std::vector<IDirect3DTexture9*> releaseList;
    EnterCriticalSection(&g_textureLock);
    for (auto& pair : g_runtimeTextures) {
        if (pair.second.replacement) releaseList.push_back(pair.second.replacement);
        pair.second.replacement = nullptr;
        pair.second.matchedHash = 0;
        pair.second.dirty = true;
        pair.second.attempted = false;
    }
    LeaveCriticalSection(&g_textureLock);
    for (IDirect3DTexture9* texture : releaseList) texture->Release();
}

static ULONG STDMETHODCALLTYPE HookTextureRelease(IDirect3DTexture9* self)
{
    ULONG count = g_realTextureRelease ? g_realTextureRelease(self) : 0;
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

static HRESULT STDMETHODCALLTYPE HookLockRect(IDirect3DTexture9* self, UINT level,
                                              D3DLOCKED_RECT* locked,
                                              const RECT* rect, DWORD flags)
{
    return g_realLockRect ? g_realLockRect(self, level, locked, rect, flags)
                          : D3DERR_INVALIDCALL;
}

static HRESULT STDMETHODCALLTYPE HookUnlockRect(IDirect3DTexture9* self, UINT level)
{
    HRESULT hr = g_realUnlockRect ? g_realUnlockRect(self, level) : D3DERR_INVALIDCALL;
    if (SUCCEEDED(hr)) MarkDirty(self);
    return hr;
}

static HRESULT STDMETHODCALLTYPE HookAddDirtyRect(IDirect3DTexture9* self, const RECT* rect)
{
    HRESULT hr = g_realAddDirtyRect ? g_realAddDirtyRect(self, rect) : D3DERR_INVALIDCALL;
    if (SUCCEEDED(hr)) MarkDirty(self);
    return hr;
}

static void PatchTexture(IDirect3DTexture9* texture)
{
    if (!texture) return;
    void** vtable = *(void***)texture;
    HookManager::PatchVtableEntry(vtable, kVtableTextureRelease,
                                  (void*)HookTextureRelease,
                                  (void**)&g_realTextureRelease,
                                  "IDirect3DTexture9::Release");
    HookManager::PatchVtableEntry(vtable, kVtableTextureLockRect,
                                  (void*)HookLockRect,
                                  (void**)&g_realLockRect,
                                  "IDirect3DTexture9::LockRect");
    HookManager::PatchVtableEntry(vtable, kVtableTextureUnlockRect,
                                  (void*)HookUnlockRect,
                                  (void**)&g_realUnlockRect,
                                  "IDirect3DTexture9::UnlockRect");
    HookManager::PatchVtableEntry(vtable, kVtableTextureAddDirtyRect,
                                  (void*)HookAddDirtyRect,
                                  (void**)&g_realAddDirtyRect,
                                  "IDirect3DTexture9::AddDirtyRect");
}

static HRESULT STDMETHODCALLTYPE HookCreateTexture(IDirect3DDevice9* self,
    UINT width, UINT height, UINT levels, DWORD usage, D3DFORMAT format,
    D3DPOOL pool, IDirect3DTexture9** output, HANDLE* sharedHandle)
{
    HRESULT hr = g_realCreateTexture
        ? g_realCreateTexture(self, width, height, levels, usage, format, pool, output, sharedHandle)
        : D3DERR_INVALIDCALL;
    if (SUCCEEDED(hr) && output && *output) {
        PatchTexture(*output);
        if (InterlockedCompareExchange(&g_internalTextureCreate, 0, 0) == 0) {
            EnterCriticalSection(&g_textureLock);
            g_runtimeTextures.emplace(*output, RuntimeTexture());
            LeaveCriticalSection(&g_textureLock);
        }
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE HookUpdateTexture(IDirect3DDevice9* self,
                                                    IDirect3DBaseTexture9* source,
                                                    IDirect3DBaseTexture9* destination)
{
    HRESULT hr = g_realUpdateTexture
        ? g_realUpdateTexture(self, source, destination) : D3DERR_INVALIDCALL;
    if (SUCCEEDED(hr) && destination && destination->GetType() == D3DRTYPE_TEXTURE)
        MarkDirty((IDirect3DTexture9*)destination);
    return hr;
}

static HRESULT STDMETHODCALLTYPE HookSetTexture(IDirect3DDevice9* self,
                                                 DWORD stage,
                                                 IDirect3DBaseTexture9* texture)
{
    IDirect3DBaseTexture9* submitted = texture;
    if (texture && texture->GetType() == D3DRTYPE_TEXTURE) {
        IDirect3DTexture9* replacement = ResolveReplacement(self, (IDirect3DTexture9*)texture);
        if (replacement) submitted = replacement;
    }
    return g_realSetTexture ? g_realSetTexture(self, stage, submitted) : D3DERR_INVALIDCALL;
}

static HRESULT STDMETHODCALLTYPE HookReset(IDirect3DDevice9* self,
                                            D3DPRESENT_PARAMETERS* parameters)
{
    ClearReplacements();
    HRESULT hr = g_realReset ? g_realReset(self, parameters) : D3DERR_INVALIDCALL;
    Log().Write(SUCCEEDED(hr) ? 2 : 1,
                "IDirect3DDevice9::Reset returned HRESULT=0x%08lX", (unsigned long)hr);
    return hr;
}

static ULONG STDMETHODCALLTYPE HookDeviceRelease(IDirect3DDevice9* self)
{
    ULONG count = g_realDeviceRelease ? g_realDeviceRelease(self) : 0;
    if (count == 0 && self == g_lastDevice) {
        g_lastDevice = nullptr;
        Log().Write(2, "Direct3D device released");
    }
    return count;
}

static void PatchDevice(IDirect3DDevice9* device)
{
    if (!device) return;
    void** vtable = *(void***)device;
    bool ok = true;
    ok &= HookManager::PatchVtableEntry(vtable, kVtableDeviceRelease,
                                         (void*)HookDeviceRelease,
                                         (void**)&g_realDeviceRelease,
                                         "IDirect3DDevice9::Release");
    ok &= HookManager::PatchVtableEntry(vtable, kVtableDeviceReset,
                                         (void*)HookReset,
                                         (void**)&g_realReset,
                                         "IDirect3DDevice9::Reset");
    ok &= HookManager::PatchVtableEntry(vtable, kVtableDeviceCreateTexture,
                                         (void*)HookCreateTexture,
                                         (void**)&g_realCreateTexture,
                                         "IDirect3DDevice9::CreateTexture");
    ok &= HookManager::PatchVtableEntry(vtable, kVtableDeviceUpdateTexture,
                                         (void*)HookUpdateTexture,
                                         (void**)&g_realUpdateTexture,
                                         "IDirect3DDevice9::UpdateTexture");
    ok &= HookManager::PatchVtableEntry(vtable, kVtableDeviceSetTexture,
                                         (void*)HookSetTexture,
                                         (void**)&g_realSetTexture,
                                         "IDirect3DDevice9::SetTexture");
    if (ok) {
        g_lastDevice = device;
        Log().Write(2, "Direct3D 9 texture hooks installed");
    }
}

static HRESULT STDMETHODCALLTYPE HookD3D9CreateDevice(IDirect3D9* self,
    UINT adapter, D3DDEVTYPE type, HWND focusWindow, DWORD behavior,
    D3DPRESENT_PARAMETERS* parameters, IDirect3DDevice9** output)
{
    HRESULT hr = g_realCreateDevice
        ? g_realCreateDevice(self, adapter, type, focusWindow, behavior, parameters, output)
        : D3DERR_INVALIDCALL;
    if (SUCCEEDED(hr) && output && *output) PatchDevice(*output);
    return hr;
}

static IDirect3D9* WINAPI HookDirect3DCreate9(UINT sdkVersion)
{
    IDirect3D9* d3d = g_realDirect3DCreate9 ? g_realDirect3DCreate9(sdkVersion) : nullptr;
    if (d3d) {
        void** vtable = *(void***)d3d;
        if (HookManager::PatchVtableEntry(vtable, kVtableD3D9CreateDevice,
                                          (void*)HookD3D9CreateDevice,
                                          (void**)&g_realCreateDevice,
                                          "IDirect3D9::CreateDevice"))
            Log().Write(2, "IDirect3D9::CreateDevice hook installed");
    }
    return d3d;
}

static void RegisterConfig()
{
    Ini().AddSection(L"TextureLoader", L"Texture Loader (experimental module)");
    Ini().AddBool(L"TextureLoader", L"Enable", false,
                  {L"Disabled by default. Set 1 to load configured TPF/ZIP packages."});
    Ini().AddBool(L"TextureLoader", L"LaterPackageWins", true,
                  {L"When two packages contain the same hash, later PackageN entries override earlier ones."});

    Ini().AddSection(L"TexturePackages", L"Texture Packages");
    Ini().AddString(L"TexturePackages", L"Package1", L"XBOX.tpf",
                    {L"Relative paths are resolved from the ASI directory. Empty entries are ignored."});
    Ini().AddString(L"TexturePackages", L"Package2", L"");
    Ini().AddString(L"TexturePackages", L"Package3", L"");
}

static void ValidateConfig()
{
    Ini().ValidateBool("TextureLoader", "Enable", 0);
    Ini().ValidateBool("TextureLoader", "LaterPackageWins", 1);
    Ini().ValidateBool("Debug", "TextureLogUnmatched", 0);
    Ini().ValidateRange("Debug", "TextureMaxUnmatchedLogs", 256, 0, 100000);
}

static bool Initialize()
{
    g_enabled = Ini().GetInt("TextureLoader", "Enable", 0) != 0;
    g_laterPackageWins = Ini().GetInt("TextureLoader", "LaterPackageWins", 1) != 0;
    g_logUnmatched = Ini().GetInt("Debug", "TextureLogUnmatched", 0) != 0;
    g_maxUnmatchedLogs = ClampInt(Ini().GetInt("Debug", "TextureMaxUnmatchedLogs", 256), 0, 100000);
    if (!g_enabled) {
        Log().Write(2, "Texture loader module disabled");
        return true;
    }

    InitializeCriticalSection(&g_textureLock);
    g_textureLockReady = true;
    LoadPackages();
    if (g_replacements.empty()) {
        Log().Write(1, "Texture loader has no usable replacement textures; D3D hooks will not be installed");
        return true;
    }
    ResolveD3DX();
    return true;
}

static void RegisterHooks()
{
    if (!g_enabled || g_replacements.empty() || !g_d3dxReady) return;
    Hooks().RegisterIatHook({"D3D9.DLL"}, "Direct3DCreate9",
                            (void*)HookDirect3DCreate9,
                            (void**)&g_realDirect3DCreate9,
                            "TextureLoader");
}

static void Start()
{
    g_d3dIatHookInstalled = Hooks().InstalledCountForOwner("TextureLoader") > 0;
    if (g_enabled && !g_replacements.empty()) {
        if (g_d3dIatHookInstalled)
            Log().Write(2, "Direct3DCreate9 texture hook installed; waiting for D3D9 device creation");
        else
            Log().Write(1, "Direct3DCreate9 texture hook was not installed");
    }
}

static void WriteReport(ReportWriter& report)
{
    report.Line(L"[TextureLoader]");
    report.Line(L"Enabled: %d", g_enabled ? 1 : 0);
    report.Line(L"Packages loaded: %d", g_loadedPackages);
    report.Line(L"Unique hashes: %u", (unsigned)g_replacements.size());
    report.Line(L"Duplicate definitions: %d", g_duplicateDefinitions);
    report.Line(L"D3DX image loader ready: %d", g_d3dxReady ? 1 : 0);
    report.Line(L"Direct3DCreate9 IAT hook installed: %d", g_d3dIatHookInstalled ? 1 : 0);
    report.Line(L"Textures replaced: %ld", (long)g_replacedTextures);
    report.Line(L"Hash mode: top-compact-complement");
    report.Line(L"");
}

static const Module kModule = {
    L"TextureLoader", RegisterConfig, ValidateConfig, Initialize,
    RegisterHooks, nullptr, Start, WriteReport
};
const Module& GetModule() { return kModule; }

} // namespace popopt::textureloader
