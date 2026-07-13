// ============================================================
// PoP_UniversalPatch.asi - Prince of Persia (2008) universal helper v28 stable
// ASI-only version: no on-disk EXE editing.
//
// Stable universal branch:
//   - GOG, Steam unpacked and original packed SteamStub support.
//   - Independent text/voice languages with transactional voice patching.
//   - Voice-package auto-detection and safe fallback.
//   - Windowed/borderless display, DPI, graphics and CPU-affinity controls.
//   - Dependency-free DirectInput Win-key fix.
//   - Focus-loss mouse release without permanently disabling capture.
//   - Config validation, compatibility reports, log levels and Unicode paths.
//
// Voice patching is transactional when AssetPatch=1:
//   Package + Resource + Bundle + Name must all match before anything is written.
//   This avoids mixed states such as English voice package + Chinese resource selectors.
//
// Build as a 32-bit DLL and name it PoP_UniversalPatch.asi or PoP_Settings.asi.
// ============================================================
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <wchar.h>
#include <wctype.h>
#include <stdlib.h>

// This build deliberately avoids including/linking DirectInput headers/libs.
// DirectInput is handled through GetProcAddress/IAT and raw COM vtable patching
// so the ASI does not gain a hard dependency on dinput8.dll or dxguid.lib.

#define LOG_PREFIX "[PoP_UniversalPatch] "

// ---- Imported function pointers for IAT hooks ----
typedef LONG   (WINAPI *RegQueryValueExA_t)(HKEY, LPCSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
typedef LONG   (WINAPI *RegQueryValueExW_t)(HKEY, LPCWSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
typedef HANDLE (WINAPI *CreateFileA_t)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
typedef HANDLE (WINAPI *CreateFileW_t)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
typedef FARPROC (WINAPI *GetProcAddress_t)(HMODULE, LPCSTR);
typedef HRESULT (WINAPI *DirectInput8Create_t)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);

static RegQueryValueExA_t Real_RegQueryValueExA = NULL;
static RegQueryValueExW_t Real_RegQueryValueExW = NULL;
static CreateFileA_t Real_CreateFileA = NULL;
static CreateFileW_t Real_CreateFileW = NULL;
static GetProcAddress_t Real_GetProcAddress = NULL;
static DirectInput8Create_t Real_DirectInput8Create = NULL;

static wchar_t g_iniPath[MAX_PATH * 4] = {0};
static wchar_t g_logPath[MAX_PATH * 4] = {0};
static wchar_t g_reportPath[MAX_PATH * 4] = {0};
static wchar_t g_asiDir[MAX_PATH * 4] = {0};
static wchar_t g_gameDir[MAX_PATH * 4] = {0};
static volatile LONG g_regHookReentry = 0;

// 0=off, 1=errors/warnings, 2=normal, 3=verbose/file tracing.
static int g_logLevel = 0;
static int g_configCorrectionCount = 0;
static int g_effectiveVoiceLanguage = -1;
static int g_voiceRuntimeEnabled = 0;
static int g_exeFlavor = 0; // 1=GOG/unpacked-like, 2=Steam unpacked, 3=packed SteamStub
static volatile LONG g_voiceFullPatchApplied = 0;
static volatile LONG g_windowPatchApplied = 0;
static int g_advapiHookCount = 0;
static int g_kernelHookCount = 0;
static int g_dinputHookCount = 0;
static int g_cpuAffinityAppliedCores = 0;
static DWORD_PTR g_cpuAffinityMask = 0;
static volatile LONG g_focusReleaseThreadStarted = 0;
static volatile LONG g_reportThreadStarted = 0;

static void WriteCompatibilityReport();

// Packed SteamStub support state.
// This stable branch does not scan the whole image for Steam Asset patches.
// It waits for SteamStub to unpack the original game code in memory, then tests
// the fixed Steam-unpacked RVAs confirmed from the unpacked executable.
static volatile LONG g_packedSteamRuntimeMode = 0;
static volatile LONG g_packedSteamVoiceApplied = 0;
static volatile LONG g_packedSteamVoiceThreadStarted = 0;
static volatile LONG g_voicePatchInProgress = 0;

static bool TryApplyPackedSteamVoicePatch(const char* reason, bool logMiss);
static void TryPackedSteamVoicePatchFast(const char* reason);
static int ReadWindowMode();

// ---- Logging ----
static void LogMessageV(int level, const char* fmt, va_list ap)
{
    if (g_logLevel < level || !g_logPath[0]) return;

    char line[1600];
#if defined(_MSC_VER)
    _vsnprintf_s(line, sizeof(line), _TRUNCATE, fmt, ap);
#else
    vsnprintf(line, sizeof(line), fmt, ap);
#endif
    line[sizeof(line) - 1] = 0;

    HANDLE f = CreateFileW(g_logPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD w = 0;
        WriteFile(f, line, (DWORD)lstrlenA(line), &w, NULL);
        WriteFile(f, "\r\n", 2, &w, NULL);
        CloseHandle(f);
    }

    OutputDebugStringA(line);
    OutputDebugStringA("\n");
}

static void LogLevel(int level, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    LogMessageV(level, fmt, ap);
    va_end(ap);
}

static void LogLine(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    LogMessageV(2, fmt, ap);
    va_end(ap);
}

static void LogWarn(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    LogMessageV(1, fmt, ap);
    va_end(ap);
}

static void LogTrace(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    LogMessageV(3, fmt, ap);
    va_end(ap);
}

static void WideToUtf8(const wchar_t* src, char* dst, int dstBytes)
{
    if (!dst || dstBytes <= 0) return;
    dst[0] = 0;
    if (!src) return;
    WideCharToMultiByte(CP_UTF8, 0, src, -1, dst, dstBytes, NULL, NULL);
    dst[dstBytes - 1] = 0;
}

// ---- INI helpers ----
static void AnsiKeyToWide(const char* src, wchar_t* dst, int count)
{
    if (!dst || count <= 0) return;
    dst[0] = 0;
    if (!src) return;
    MultiByteToWideChar(CP_ACP, 0, src, -1, dst, count);
    dst[count - 1] = 0;
}

static int ReadIniInt(const char* section, const char* key, int def)
{
    if (!g_iniPath[0]) return def;
    wchar_t ws[128], wk[128];
    AnsiKeyToWide(section, ws, 128);
    AnsiKeyToWide(key, wk, 128);
    return GetPrivateProfileIntW(ws, wk, def, g_iniPath);
}

static void WriteIniInt(const char* section, const char* key, int value)
{
    wchar_t ws[128], wk[128], wv[64];
    AnsiKeyToWide(section, ws, 128);
    AnsiKeyToWide(key, wk, 128);
    wsprintfW(wv, L"%d", value);
    WritePrivateProfileStringW(ws, wk, wv, g_iniPath);
}

static int ClampInt(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static int ReadTextLanguage()
{
    // No legacy config alias here by design. The game still queries the registry
    // value named LNG_Language; this ASI answers it with TextLanguage.
    return ClampInt(ReadIniInt("Language", "TextLanguage", 6), 0, 13);
}

static int ReadConfiguredVoiceLanguage()
{
    return ClampInt(ReadIniInt("Language", "VoiceLanguage", 1), 1, 13);
}

static int ReadVoiceLanguage()
{
    if (g_effectiveVoiceLanguage >= 1 && g_effectiveVoiceLanguage <= 13)
        return g_effectiveVoiceLanguage;
    return ReadConfiguredVoiceLanguage();
}

static const wchar_t* VoiceLanguageNameW(int id)
{
    static const wchar_t* names[] = {
        L"None/Auto", L"English", L"French", L"Spanish", L"Polish", L"German",
        L"Chinese", L"Hungarian", L"Italian", L"Japanese", L"Czech",
        L"Korean", L"Russian", L"Dutch"
    };
    return (id >= 0 && id <= 13) ? names[id] : L"Unknown";
}

static const wchar_t* VoiceLanguageSuffixW(int id)
{
    static const wchar_t* suffixes[] = {
        L"", L"Eng", L"Fre", L"Spa", L"Pol", L"Ger", L"Chi",
        L"Hun", L"Ita", L"Jap", L"Cze", L"Kor", L"Rus", L"Dut"
    };
    return (id >= 1 && id <= 13) ? suffixes[id] : L"";
}

static void BuildVoicePackPathW(int language, wchar_t* out, int count)
{
    if (!out || count <= 0) return;
    out[0] = 0;
    const wchar_t* suffix = VoiceLanguageSuffixW(language);
    if (!suffix || !suffix[0]) return;
#if defined(_MSC_VER)
    _snwprintf_s(out, count, _TRUNCATE, L"%ls\\DataPC_StreamedSounds%ls.forge", g_gameDir, suffix);
#else
    swprintf(out, count, L"%ls\\DataPC_StreamedSounds%ls.forge", g_gameDir, suffix);
#endif
    out[count - 1] = 0;
}

static bool VoicePackExists(int language)
{
    wchar_t path[MAX_PATH * 4] = {0};
    BuildVoicePackPathW(language, path, MAX_PATH * 4);
    if (!path[0]) return false;
    DWORD attr = GetFileAttributesW(path);
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

static void ResolveVoiceLanguageAndFallback()
{
    int enabled = ReadIniInt("Voice", "Enable", 1) ? 1 : 0;
    int requested = ReadConfiguredVoiceLanguage();
    int autoFallback = ReadIniInt("Voice", "AutoFallback", 1) ? 1 : 0;
    int fallback = ClampInt(ReadIniInt("Voice", "FallbackLanguage", 1), 1, 13);

    g_effectiveVoiceLanguage = requested;
    g_voiceRuntimeEnabled = enabled;
    if (!enabled) return;

    if (VoicePackExists(requested)) {
        LogLine(LOG_PREFIX "Voice pack detected: requested language=%d", requested);
        return;
    }

    if (autoFallback && fallback != requested && VoicePackExists(fallback)) {
        g_effectiveVoiceLanguage = fallback;
        LogWarn(LOG_PREFIX "Requested voice pack language=%d is missing; safely falling back to language=%d", requested, fallback);
        return;
    }

    g_effectiveVoiceLanguage = -1;
    g_voiceRuntimeEnabled = 0;
    if (autoFallback)
        LogWarn(LOG_PREFIX "Requested voice pack language=%d is missing and fallback language=%d is unavailable; voice patches disabled", requested, fallback);
    else
        LogWarn(LOG_PREFIX "Requested voice pack language=%d is missing and AutoFallback=0; voice patches disabled", requested);
}

static char LowerAscii(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

static bool StrStrIA_Simple(const char* s, const char* needle)
{
    if (!s || !needle || !*needle) return false;
    size_t nlen = lstrlenA(needle);
    for (const char* p = s; *p; ++p) {
        size_t i = 0;
        while (i < nlen && p[i] && LowerAscii(p[i]) == LowerAscii(needle[i])) ++i;
        if (i == nlen) return true;
    }
    return false;
}

static bool ShouldTraceFileA(const char* path)
{
    if (!path) return false;
    return StrStrIA_Simple(path, ".forge") ||
           StrStrIA_Simple(path, "Streamed") ||
           StrStrIA_Simple(path, "Sound") ||
           StrStrIA_Simple(path, "DataPC");
}

// ---- Registry values intercepted from PoP_UniversalPatch.ini ----
// The real game still asks for Ubisoft's original registry value names.
// This layer translates those names to a cleaner public config layout.
static DWORD ReadAspectRatioOverride()
{
    // In the original registry, AspectRatioOverride=0 behaves like widescreen override off.
    // PCGW-style value is roughly: (desired width / desired height) * 100, stored as
    // a decimal DWORD/integer in the registry. AspectRatio=0 means auto-compute it
    // from [Display] Width/Height using integer division.
    int widescreen = ReadIniInt("Display", "Widescreen", 1) ? 1 : 0;
    if (!widescreen) return 0;

    int aspect = ReadIniInt("Display", "AspectRatio", 0);
    if (aspect <= 0) {
        int width = ReadIniInt("Display", "Width", 1920);
        int height = ReadIniInt("Display", "Height", 1080);
        if (height > 0 && width > 0)
            aspect = (width * 100) / height; // floor: 3840/2160*100 -> 177
        else
            aspect = 177;
    }

    return (DWORD)ClampInt(aspect, 0, 999);
}

static DWORD ReadHighResolutionTexturesAsDegradedTextures()
{
    // Game key: DegradedTextures. 0 = high-resolution textures enabled, 1 = degraded/low textures.
    // Public key: HighResolutionTextures. 1 = enabled, 0 = disabled.
    int highRes = ReadIniInt("Graphics", "HighResolutionTextures", 1) ? 1 : 0;
    return highRes ? 0u : 1u;
}

static DWORD ReadHighResolutionTexturesAsLauncherTextureLevel()
{
    // Launcher key: Texture_lvl. 1 = high, 0 = low.
    int highRes = ReadIniInt("Graphics", "HighResolutionTextures", 1) ? 1 : 0;
    return highRes ? 1u : 0u;
}

typedef struct { const char* regName; const char* section; const char* key; int def; } RegistryEntry;
static const RegistryEntry kRegistryEntries[] = {
    {"LNG_Language",          "Language",         "TextLanguage",             6},
    {"ScreenResolutionWidth", "Display",          "Width",                    3840},
    {"ScreenResolutionHeight","Display",          "Height",                   2160},
    {"VerticalSync",          "Display",          "VSync",                    1},
    {"Antialiasing",          "Graphics",         "AntiAliasing",             4},
    {"Shadows",               "AdvancedGraphics", "ShadowQuality",            2},
    {"PostEffects",           "AdvancedGraphics", "PostEffects",              2},
    {"VisualQualityLvl",      "Graphics",         "Quality",                  2},
};
static const int kRegistryEntryCount = sizeof(kRegistryEntries) / sizeof(kRegistryEntries[0]);

static bool LookupConfigRegistryValueA(LPCSTR name, DWORD* outVal)
{
    if (!name || !outVal) return false;

    // Computed mappings whose public meaning intentionally differs from the original key.
    if (lstrcmpiA(name, "AspectRatioOverride") == 0) {
        *outVal = ReadAspectRatioOverride();
        return true;
    }
    if (lstrcmpiA(name, "DegradedTextures") == 0) {
        *outVal = ReadHighResolutionTexturesAsDegradedTextures();
        return true;
    }
    if (lstrcmpiA(name, "Texture_lvl") == 0) {
        *outVal = ReadHighResolutionTexturesAsLauncherTextureLevel();
        return true;
    }

    // Extra defensive aliases. They may not all be queried by this build, but returning
    // Widescreen here is harmless if a build/launcher asks for them.
    if (lstrcmpiA(name, "Widescreen") == 0 ||
        lstrcmpiA(name, "WideScreen") == 0 ||
        lstrcmpiA(name, "Platform_PCWidescreen") == 0) {
        *outVal = (DWORD)(ReadIniInt("Display", "Widescreen", 1) ? 1 : 0);
        return true;
    }

    for (int i = 0; i < kRegistryEntryCount; ++i) {
        if (lstrcmpiA(name, kRegistryEntries[i].regName) == 0) {
            int iniVal = ReadIniInt(kRegistryEntries[i].section, kRegistryEntries[i].key, kRegistryEntries[i].def);
            if (lstrcmpiA(kRegistryEntries[i].regName, "LNG_Language") == 0)
                iniVal = ReadTextLanguage();
            if (iniVal == -1) return false; // -1 means let the original registry value pass through.
            *outVal = (DWORD)iniVal;
            return true;
        }
    }

    return false;
}

// ---- Registry hooks ----
LONG WINAPI Hook_RegQueryValueExA(HKEY hKey, LPCSTR lpValueName, LPDWORD reserved,
                                  LPDWORD lpType, LPBYTE lpData, LPDWORD lpcbData)
{
    if (InterlockedExchange(&g_regHookReentry, 1)) {
        return Real_RegQueryValueExA ? Real_RegQueryValueExA(hKey, lpValueName, reserved, lpType, lpData, lpcbData)
                                     : ERROR_FILE_NOT_FOUND;
    }

    if (lpValueName && lstrcmpiA(lpValueName, "LNG_Language") == 0)
        TryPackedSteamVoicePatchFast("RegQueryValueExA(LNG_Language)");

    DWORD val = 0;
    bool intercepted = LookupConfigRegistryValueA(lpValueName, &val);
    LONG result = ERROR_SUCCESS;

    if (intercepted) {
        if (lpType) *lpType = REG_DWORD;
        if (lpData && lpcbData && *lpcbData >= sizeof(DWORD))
            *(DWORD*)lpData = val;
        if (lpcbData) *lpcbData = sizeof(DWORD);
    } else {
        result = Real_RegQueryValueExA ? Real_RegQueryValueExA(hKey, lpValueName, reserved, lpType, lpData, lpcbData)
                                       : ERROR_FILE_NOT_FOUND;
    }

    InterlockedExchange(&g_regHookReentry, 0);
    return result;
}

LONG WINAPI Hook_RegQueryValueExW(HKEY hKey, LPCWSTR lpValueName, LPDWORD reserved,
                                  LPDWORD lpType, LPBYTE lpData, LPDWORD lpcbData)
{
    if (InterlockedExchange(&g_regHookReentry, 1)) {
        return Real_RegQueryValueExW ? Real_RegQueryValueExW(hKey, lpValueName, reserved, lpType, lpData, lpcbData)
                                     : ERROR_FILE_NOT_FOUND;
    }

    char nameA[256] = {0};
    if (lpValueName)
        WideCharToMultiByte(CP_ACP, 0, lpValueName, -1, nameA, sizeof(nameA), NULL, NULL);

    if (nameA[0] && lstrcmpiA(nameA, "LNG_Language") == 0)
        TryPackedSteamVoicePatchFast("RegQueryValueExW(LNG_Language)");

    DWORD val = 0;
    bool intercepted = LookupConfigRegistryValueA(nameA, &val);
    LONG result = ERROR_SUCCESS;

    if (intercepted) {
        if (lpType) *lpType = REG_DWORD;
        if (lpData && lpcbData && *lpcbData >= sizeof(DWORD))
            *(DWORD*)lpData = val;
        if (lpcbData) *lpcbData = sizeof(DWORD);
    } else {
        result = Real_RegQueryValueExW ? Real_RegQueryValueExW(hKey, lpValueName, reserved, lpType, lpData, lpcbData)
                                       : ERROR_FILE_NOT_FOUND;
    }

    InterlockedExchange(&g_regHookReentry, 0);
    return result;
}

// ---- Optional file-open tracing ----
HANDLE WINAPI Hook_CreateFileA(LPCSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode,
                               LPSECURITY_ATTRIBUTES lpSecurityAttributes, DWORD dwCreationDisposition,
                               DWORD dwFlagsAndAttributes, HANDLE hTemplateFile)
{
    if (lpFileName && ShouldTraceFileA(lpFileName))
        TryPackedSteamVoicePatchFast("CreateFileA");

    HANDLE h = Real_CreateFileA ? Real_CreateFileA(lpFileName, dwDesiredAccess, dwShareMode, lpSecurityAttributes,
                                                   dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile)
                              : INVALID_HANDLE_VALUE;

    if (g_logLevel >= 3 && ShouldTraceFileA(lpFileName)) {
        LogLine(LOG_PREFIX "CreateFileA %s -> %s", lpFileName ? lpFileName : "(null)",
                h == INVALID_HANDLE_VALUE ? "FAIL" : "OK");
    }
    return h;
}

HANDLE WINAPI Hook_CreateFileW(LPCWSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode,
                               LPSECURITY_ATTRIBUTES lpSecurityAttributes, DWORD dwCreationDisposition,
                               DWORD dwFlagsAndAttributes, HANDLE hTemplateFile)
{
    if (lpFileName) {
        char voiceProbe[MAX_PATH * 2] = {0};
        WideCharToMultiByte(CP_UTF8, 0, lpFileName, -1, voiceProbe, sizeof(voiceProbe), NULL, NULL);
        if (ShouldTraceFileA(voiceProbe))
            TryPackedSteamVoicePatchFast("CreateFileW");
    }

    HANDLE h = Real_CreateFileW ? Real_CreateFileW(lpFileName, dwDesiredAccess, dwShareMode, lpSecurityAttributes,
                                                   dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile)
                              : INVALID_HANDLE_VALUE;

    if (g_logLevel >= 3 && lpFileName) {
        char buf[MAX_PATH * 2] = {0};
        WideCharToMultiByte(CP_UTF8, 0, lpFileName, -1, buf, sizeof(buf), NULL, NULL);
        if (ShouldTraceFileA(buf)) {
            LogLine(LOG_PREFIX "CreateFileW %s -> %s", buf,
                    h == INVALID_HANDLE_VALUE ? "FAIL" : "OK");
        }
    }
    return h;
}


// ---- Windowed input fixes ----
static bool IsWindowedInputFixAllowed()
{
    // Fullscreen keeps the game's original input behavior. Windowed and borderless
    // modes may opt out of Win-key suppression and mouse confinement.
    return ReadWindowMode() != 0;
}

static bool ShouldAllowWinKeyInWindowed()
{
    return IsWindowedInputFixAllowed() && (ReadIniInt("Input", "AllowWinKeyInWindowed", 1) ? true : false);
}

static bool ShouldReleaseMouseOnFocusLoss()
{
    return IsWindowedInputFixAllowed() && (ReadIniInt("Input", "ReleaseMouseOnFocusLoss", 1) ? true : false);
}

static bool GuidEqualLocal(REFGUID a, const GUID& b)
{
    return memcmp(&a, &b, sizeof(GUID)) == 0;
}

// Avoid DirectInput SDK/lib dependencies by defining only the tiny constants we need.
static const DWORD DISCL_NOWINKEY_LOCAL = 0x00000010;
static const GUID kIidDirectInput8ALocal =
    { 0xBF798030, 0x483A, 0x4DA2, { 0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00 } };

typedef HRESULT (STDMETHODCALLTYPE *DI8_CreateDevice_t)(void* self, REFGUID rguid, void** outDevice, IUnknown* outer);
typedef HRESULT (STDMETHODCALLTYPE *DIDevice_SetCooperativeLevel_t)(void* self, HWND hwnd, DWORD flags);

static DI8_CreateDevice_t Real_DI8_CreateDevice = NULL;
static DIDevice_SetCooperativeLevel_t Real_DIDevice_SetCooperativeLevel = NULL;
static volatile LONG g_diCreateDeviceHooked = 0;
static volatile LONG g_diSetCoopHooked = 0;

static bool PatchVTableEntry(void** vtbl, int index, void* replacement, void** realSlot, const char* label)
{
    if (!vtbl || !replacement) return false;

    void** slot = &vtbl[index];
    void* cur = *slot;
    if (cur == replacement) return true;

    // Keep this simple and safe. If a different hook already replaced the slot, do not chain.
    if (realSlot && *realSlot && cur != *realSlot) {
        static volatile LONG s_loggedConflict = 0;
        if (InterlockedCompareExchange(&s_loggedConflict, 1, 0) == 0)
            LogLine(LOG_PREFIX "%s vtable slot already hooked by another module; skipped", label ? label : "DirectInput");
        return false;
    }

    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &old))
        return false;

    if (realSlot && !*realSlot)
        *realSlot = cur;
    *slot = replacement;

    DWORD tmp = 0;
    VirtualProtect(slot, sizeof(void*), old, &tmp);
    FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));
    return true;
}

HRESULT STDMETHODCALLTYPE Hook_DIDevice_SetCooperativeLevel(void* self, HWND hwnd, DWORD flags)
{
    if (ShouldAllowWinKeyInWindowed() && (flags & DISCL_NOWINKEY_LOCAL)) {
        flags &= ~DISCL_NOWINKEY_LOCAL;
        static volatile LONG s_logged = 0;
        if (InterlockedCompareExchange(&s_logged, 1, 0) == 0)
            LogLine(LOG_PREFIX "DirectInput DISCL_NOWINKEY cleared in windowed/borderless mode");
    }
    return Real_DIDevice_SetCooperativeLevel ? Real_DIDevice_SetCooperativeLevel(self, hwnd, flags) : E_FAIL;
}

HRESULT STDMETHODCALLTYPE Hook_DI8_CreateDevice(void* self, REFGUID rguid, void** outDevice, IUnknown* outer)
{
    HRESULT hr = Real_DI8_CreateDevice ? Real_DI8_CreateDevice(self, rguid, outDevice, outer) : E_FAIL;

    if (SUCCEEDED(hr) && outDevice && *outDevice && ShouldAllowWinKeyInWindowed()) {
        void** devVtbl = *(void***)(*outDevice);
        if (PatchVTableEntry(devVtbl, 13, (void*)Hook_DIDevice_SetCooperativeLevel,
                             (void**)&Real_DIDevice_SetCooperativeLevel,
                             "DirectInputDevice8::SetCooperativeLevel")) {
            if (InterlockedCompareExchange(&g_diSetCoopHooked, 1, 0) == 0)
                LogLine(LOG_PREFIX "DirectInput device SetCooperativeLevel vtable hooked for Win-key fix");
        }
    }
    return hr;
}

static void TryPatchDirectInput8Object(void* directInputObj)
{
    if (!directInputObj || !ShouldAllowWinKeyInWindowed()) return;
    void** vtbl = *(void***)directInputObj;
    if (PatchVTableEntry(vtbl, 3, (void*)Hook_DI8_CreateDevice,
                         (void**)&Real_DI8_CreateDevice,
                         "IDirectInput8::CreateDevice")) {
        if (InterlockedCompareExchange(&g_diCreateDeviceHooked, 1, 0) == 0)
            LogLine(LOG_PREFIX "DirectInput8 CreateDevice vtable hooked for Win-key fix");
    }
}

HRESULT WINAPI Hook_DirectInput8Create(HINSTANCE hinst, DWORD version, REFIID riid, LPVOID* out, LPUNKNOWN outer)
{
    if (!Real_DirectInput8Create) return E_FAIL;

    HRESULT hr = Real_DirectInput8Create(hinst, version, riid, out, outer);
    if (SUCCEEDED(hr) && out && *out && ShouldAllowWinKeyInWindowed()) {
        // Usually IID_IDirectInput8A, but vtable layout is the same for the interface we need.
        if (!GuidEqualLocal(riid, kIidDirectInput8ALocal)) {
            static volatile LONG s_loggedNonA = 0;
            if (InterlockedCompareExchange(&s_loggedNonA, 1, 0) == 0)
                LogLine(LOG_PREFIX "DirectInput8Create returned non-A IID; attempting vtable hook anyway");
        }
        TryPatchDirectInput8Object(*out);
        static volatile LONG s_logged = 0;
        if (InterlockedCompareExchange(&s_logged, 1, 0) == 0)
            LogLine(LOG_PREFIX "DirectInput8Create intercepted without hard dinput dependency");
    }
    return hr;
}

static bool ModuleNameContains(HMODULE mod, const char* needle)
{
    wchar_t path[MAX_PATH * 4] = {0};
    wchar_t wneedle[128] = {0};
    if (!mod || !needle) return false;
    if (!GetModuleFileNameW(mod, path, MAX_PATH * 4)) return false;
    MultiByteToWideChar(CP_ACP, 0, needle, -1, wneedle, 128);

    for (wchar_t* p = path; *p; ++p) {
        int i = 0;
        while (wneedle[i] && p[i] && towlower(p[i]) == towlower(wneedle[i])) ++i;
        if (!wneedle[i]) return true;
    }
    return false;
}

FARPROC WINAPI Hook_GetProcAddress(HMODULE hModule, LPCSTR lpProcName)
{
    FARPROC p = Real_GetProcAddress ? Real_GetProcAddress(hModule, lpProcName) : NULL;
    if (!lpProcName || ((ULONG_PTR)lpProcName <= 0xFFFF) || !p)
        return p;

    if (lstrcmpiA(lpProcName, "DirectInput8Create") == 0 && ModuleNameContains(hModule, "dinput8")) {
        Real_DirectInput8Create = (DirectInput8Create_t)p;
        static volatile LONG s_logged = 0;
        if (InterlockedCompareExchange(&s_logged, 1, 0) == 0)
            LogLine(LOG_PREFIX "GetProcAddress hook: DirectInput8Create wrapped");
        return (FARPROC)Hook_DirectInput8Create;
    }

    return p;
}

// ---- PE and patch helpers ----
static bool GetImageRange(HMODULE mod, BYTE** baseOut, DWORD* sizeOut)
{
    if (!mod || !baseOut || !sizeOut) return false;
    BYTE* base = (BYTE*)mod;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    *baseOut = base;
    *sizeOut = nt->OptionalHeader.SizeOfImage;
    return true;
}

static BYTE* FindBytes(BYTE* base, DWORD size, const BYTE* pattern, DWORD patternSize)
{
    if (!base || !pattern || !patternSize || size < patternSize) return NULL;
    for (DWORD i = 0; i <= size - patternSize; ++i) {
        if (memcmp(base + i, pattern, patternSize) == 0)
            return base + i;
    }
    return NULL;
}

static bool MatchMaskAt(const BYTE* p, const BYTE* pattern, const char* mask, DWORD size)
{
    if (!p || !pattern || !mask) return false;
    for (DWORD i = 0; i < size; ++i) {
        if (mask[i] == 'x' && p[i] != pattern[i]) return false;
    }
    return true;
}

static BYTE* FindBytesMasked(BYTE* base, DWORD size, const BYTE* pattern, const char* mask, DWORD patternSize)
{
    if (!base || !pattern || !mask || !patternSize || size < patternSize) return NULL;
    for (DWORD i = 0; i <= size - patternSize; ++i) {
        if (MatchMaskAt(base + i, pattern, mask, patternSize))
            return base + i;
    }
    return NULL;
}

typedef struct {
    const char* label;
    DWORD rva;
} RvaCandidate;

static bool HasSectionNamed(const char* wanted)
{
    HMODULE mod = GetModuleHandleA(NULL);
    if (!mod || !wanted) return false;

    BYTE* base = (BYTE*)mod;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        char name[9] = {0};
        memcpy(name, sec[i].Name, 8);
        if (lstrcmpiA(name, wanted) == 0) return true;
    }
    return false;
}

static DWORD GetEntryPointRva()
{
    HMODULE mod = GetModuleHandleA(NULL);
    if (!mod) return 0;
    BYTE* base = (BYTE*)mod;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    return nt->OptionalHeader.AddressOfEntryPoint;
}

static void LogExecutableFlavor()
{
    bool hasBind = HasSectionNamed(".bind");
    bool hasExtra = HasSectionNamed(".extra");
    DWORD ep = GetEntryPointRva();

    if (hasBind) {
        g_exeFlavor = 3;
        LogLine(LOG_PREFIX "executable flavor: packed SteamStub detected (entry RVA 0x%08X); fixed-RVA runtime voice patching will be used", (unsigned)ep);
    } else if (hasExtra) {
        g_exeFlavor = 2;
        LogLine(LOG_PREFIX "executable flavor: Steam unpacked detected (entry RVA 0x%08X)", (unsigned)ep);
    } else {
        g_exeFlavor = 1;
        LogLine(LOG_PREFIX "executable flavor: GOG/unpacked-like detected (entry RVA 0x%08X)", (unsigned)ep);
    }
}

static bool IsPackedSteamStub()
{
    return HasSectionNamed(".bind");
}

static BYTE* ResolvePatchTarget(const char* name, DWORD gogRva, const BYTE* expected, DWORD expectedSize)
{
    HMODULE mod = GetModuleHandleA(NULL);
    BYTE* base = NULL;
    DWORD imageSize = 0;
    if (!mod || !GetImageRange(mod, &base, &imageSize)) return NULL;

    if (gogRva + expectedSize <= imageSize) {
        BYTE* byRva = base + gogRva;
        if (memcmp(byRva, expected, expectedSize) == 0)
            return byRva;
    }

    BYTE* scanned = FindBytes(base, imageSize, expected, expectedSize);
    if (scanned) {
        LogLine(LOG_PREFIX "%s: RVA mismatch, pattern found at RVA 0x%08X", name, (unsigned)(scanned - base));
        return scanned;
    }

    LogLine(LOG_PREFIX "%s: expected bytes not found", name);
    return NULL;
}

static BYTE* ResolvePatchTargetCandidatesMasked(const char* name,
                                                const RvaCandidate* candidates, int candidateCount,
                                                const BYTE* expected, const char* mask, DWORD expectedSize,
                                                bool allowFullScan)
{
    HMODULE mod = GetModuleHandleA(NULL);
    BYTE* base = NULL;
    DWORD imageSize = 0;
    if (!mod || !GetImageRange(mod, &base, &imageSize)) return NULL;

    for (int i = 0; i < candidateCount; ++i) {
        DWORD rva = candidates[i].rva;
        if (rva + expectedSize <= imageSize) {
            BYTE* byRva = base + rva;
            if (MatchMaskAt(byRva, expected, mask, expectedSize)) {
                LogLine(LOG_PREFIX "%s: matched %s RVA 0x%08X", name, candidates[i].label, (unsigned)rva);
                return byRva;
            }
        }
    }

    if (allowFullScan) {
        BYTE* scanned = FindBytesMasked(base, imageSize, expected, mask, expectedSize);
        if (scanned) {
            LogLine(LOG_PREFIX "%s: candidate mismatch, wildcard pattern found at RVA 0x%08X", name, (unsigned)(scanned - base));
            return scanned;
        }
    }

    LogLine(LOG_PREFIX "%s: expected bytes not found in supported static candidates", name);
    return NULL;
}

static bool WritePatch(const char* name, BYTE* dst, const BYTE* patch, DWORD patchSize)
{
    if (!dst || !patch || !patchSize) return false;

    DWORD oldProtect = 0;
    if (!VirtualProtect(dst, patchSize, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        LogLine(LOG_PREFIX "%s: VirtualProtect failed, GetLastError=%lu", name, GetLastError());
        return false;
    }

    memcpy(dst, patch, patchSize);
    VirtualProtect(dst, patchSize, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), dst, patchSize);

    HMODULE mod = GetModuleHandleA(NULL);
    BYTE* base = (BYTE*)mod;
    LogLine(LOG_PREFIX "%s: patched at RVA 0x%08X", name, (unsigned)(dst - base));
    return true;
}

static bool PatchByRVAOrScan(const char* name, DWORD gogRva,
                             const BYTE* expected, DWORD expectedSize,
                             const BYTE* patch, DWORD patchSize)
{
    BYTE* dst = ResolvePatchTarget(name, gogRva, expected, expectedSize);
    if (!dst) {
        LogLine(LOG_PREFIX "%s: patch skipped", name);
        return false;
    }
    return WritePatch(name, dst, patch, patchSize);
}

static bool PatchByCandidatesMasked(const char* name,
                                    const RvaCandidate* candidates, int candidateCount,
                                    const BYTE* expected, const char* mask, DWORD expectedSize,
                                    const BYTE* patch, DWORD patchSize,
                                    bool allowFullScan)
{
    BYTE* dst = ResolvePatchTargetCandidatesMasked(name, candidates, candidateCount, expected, mask, expectedSize, allowFullScan);
    if (!dst) {
        LogLine(LOG_PREFIX "%s: patch skipped", name);
        return false;
    }
    return WritePatch(name, dst, patch, patchSize);
}

typedef struct {
    const char* name;
    DWORD rva;
    const RvaCandidate* candidates;
    int candidateCount;
    const BYTE* expected;
    const char* mask;
    DWORD expectedSize;
    const BYTE* patch;
    DWORD patchSize;
    bool allowFullScan;
    BYTE* target;
    DWORD oldProtect;
} PatchItem;

// ---- Voice patch database ----
// These are fixed RVAs for supported builds. Packed SteamStub uses the same
// runtime RVAs as the Steam-unpacked executable after the stub restores .text.
static const RvaCandidate kVoicePackageRvasAll[] = {
    {"GOG",           0x006C7870},
    {"SteamUnpacked", 0x006C7610},
};
static const RvaCandidate kVoiceResourceRvasAll[] = {
    {"GOG",           0x008DE8FE},
    {"SteamUnpacked", 0x008DE6FE},
};
static const RvaCandidate kVoiceBundleRvasAll[] = {
    {"GOG",           0x004F24B4},
    {"SteamUnpacked", 0x004F1CA4},
};
static const RvaCandidate kVoiceNameRvasAll[] = {
    {"GOG",           0x004F2B5F},
    {"SteamUnpacked", 0x004F234F},
};

static const RvaCandidate kVoicePackageRvasSteamOnly[] = { {"SteamRuntime", 0x006C7610} };
static const RvaCandidate kVoiceResourceRvasSteamOnly[] = { {"SteamRuntime", 0x008DE6FE} };
static const RvaCandidate kVoiceBundleRvasSteamOnly[] = { {"SteamRuntime", 0x004F1CA4} };
static const RvaCandidate kVoiceNameRvasSteamOnly[] = { {"SteamRuntime", 0x004F234F} };

static const BYTE kExpectedVoicePackage[] = {
    0x8B,0x44,0x24,0x08, 0x85,0xC0, 0x75,0x05, 0xE8,0xB3,0x74,0xA5,0xFF
};
static const char kMaskVoicePackage[] = "xxxxxxxxx????";

static const BYTE kExpectedVoiceResource[] = {
    0xE8,0x2D,0x04,0x84,0xFF, 0x50, 0xE8,0x67,0x04,0x84,0xFF,
    0x8A,0x16,0x83,0xC4,0x04,0x3A,0x10,0x75,0x14,0x8A,0x4E,0x01,
    0x3A,0x48,0x01,0x75,0x0C,0x8A,0x56,0x02,0x3A,0x50,0x02
};
static const char kMaskVoiceResource[] = "x????xx????xxxxxxxxxxxxxxxxxxxxxxx";

static const BYTE kExpectedVoiceBundle[] = {
    0xE8,0x77,0xC8,0xC2,0xFF, 0x3B,0xF8, 0x75,0x70,
    0x8B,0x15,0x24,0x74,0xF3,0x00, 0x52,0x6A,0xFF,0x53,
    0xE8,0xA4,0x01,0xC3,0xFF, 0x8B,0x0D,0x98,0x55,0xF9,0x00
};
static const char kMaskVoiceBundle[] = "x????xxx?xxxxxxxxxxx????xxxxxx";

static const BYTE kExpectedVoiceName[] = {
    0xE8,0xCC,0xC1,0xC2,0xFF, 0x50, 0xE8,0x06,0xC2,0xC2,0xFF,
    0x50,0xE8,0x70,0xF5,0x38,0x00, 0x8D,0x4C,0x24,0x18,
    0x68,0xFF,0xFF,0xFF,0x7F,0x51,0xE8,0x31,0xC2,0x38,0x00
};
static const char kMaskVoiceName[] = "x????xx????xx????xxxxxxxxxxx????";

static void BuildVoicePatchBytes(int voiceLang,
                                 BYTE patchPackage[13],
                                 BYTE patchResource[5],
                                 BYTE patchBundle[5],
                                 BYTE patchName[5])
{
    BYTE packageTemplate[13] = {
        0xB8,0x01,0x00,0x00,0x00, 0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90
    };
    BYTE movTemplate[5] = { 0xB8,0x01,0x00,0x00,0x00 };
    memcpy(patchPackage, packageTemplate, 13);
    memcpy(patchResource, movTemplate, 5);
    memcpy(patchBundle, movTemplate, 5);
    memcpy(patchName, movTemplate, 5);
    *(DWORD*)(patchPackage + 1) = (DWORD)voiceLang;
    *(DWORD*)(patchResource + 1) = (DWORD)voiceLang;
    *(DWORD*)(patchBundle + 1) = (DWORD)voiceLang;
    *(DWORD*)(patchName + 1) = (DWORD)voiceLang;
}

static BYTE* FindMatchedCandidateExactSilent(const RvaCandidate* candidates, int candidateCount,
                                             const BYTE* expected, const char* mask, DWORD expectedSize)
{
    HMODULE mod = GetModuleHandleA(NULL);
    BYTE* base = NULL;
    DWORD imageSize = 0;
    if (!mod || !GetImageRange(mod, &base, &imageSize)) return NULL;

    for (int i = 0; i < candidateCount; ++i) {
        DWORD rva = candidates[i].rva;
        if (rva + expectedSize <= imageSize) {
            BYTE* p = base + rva;
            if (MatchMaskAt(p, expected, mask, expectedSize))
                return p;
        }
    }
    return NULL;
}

static bool VoiceFullPatchCandidateSetReady(const RvaCandidate* packageRvas, int packageCount,
                                            const RvaCandidate* resourceRvas, int resourceCount,
                                            const RvaCandidate* bundleRvas, int bundleCount,
                                            const RvaCandidate* nameRvas, int nameCount)
{
    return
        FindMatchedCandidateExactSilent(packageRvas, packageCount, kExpectedVoicePackage, kMaskVoicePackage, sizeof(kExpectedVoicePackage)) &&
        FindMatchedCandidateExactSilent(resourceRvas, resourceCount, kExpectedVoiceResource, kMaskVoiceResource, sizeof(kExpectedVoiceResource)) &&
        FindMatchedCandidateExactSilent(bundleRvas, bundleCount, kExpectedVoiceBundle, kMaskVoiceBundle, sizeof(kExpectedVoiceBundle)) &&
        FindMatchedCandidateExactSilent(nameRvas, nameCount, kExpectedVoiceName, kMaskVoiceName, sizeof(kExpectedVoiceName));
}

static bool ApplyPatchGroupAtomic(const char* groupName, PatchItem* items, int count);

static bool ApplyVoiceFullPatchWithCandidateSets(const char* groupName,
                                                 const RvaCandidate* packageRvas, int packageCount,
                                                 const RvaCandidate* resourceRvas, int resourceCount,
                                                 const RvaCandidate* bundleRvas, int bundleCount,
                                                 const RvaCandidate* nameRvas, int nameCount,
                                                 int voiceLang, bool allowPackageFullScan)
{
    BYTE patchPackage[13];
    BYTE patchResource[5];
    BYTE patchBundle[5];
    BYTE patchName[5];
    BuildVoicePatchBytes(voiceLang, patchPackage, patchResource, patchBundle, patchName);

    PatchItem items[4];
    ZeroMemory(items, sizeof(items));

    items[0].name = "VoicePackageLanguage";
    items[0].candidates = packageRvas;
    items[0].candidateCount = packageCount;
    items[0].expected = kExpectedVoicePackage;
    items[0].mask = kMaskVoicePackage;
    items[0].expectedSize = sizeof(kExpectedVoicePackage);
    items[0].patch = patchPackage;
    items[0].patchSize = sizeof(patchPackage);
    items[0].allowFullScan = allowPackageFullScan;

    items[1].name = "VoiceResourceLanguage";
    items[1].candidates = resourceRvas;
    items[1].candidateCount = resourceCount;
    items[1].expected = kExpectedVoiceResource;
    items[1].mask = kMaskVoiceResource;
    items[1].expectedSize = sizeof(kExpectedVoiceResource);
    items[1].patch = patchResource;
    items[1].patchSize = sizeof(patchResource);
    items[1].allowFullScan = false;

    items[2].name = "VoiceBundleLanguage";
    items[2].candidates = bundleRvas;
    items[2].candidateCount = bundleCount;
    items[2].expected = kExpectedVoiceBundle;
    items[2].mask = kMaskVoiceBundle;
    items[2].expectedSize = sizeof(kExpectedVoiceBundle);
    items[2].patch = patchBundle;
    items[2].patchSize = sizeof(patchBundle);
    items[2].allowFullScan = false;

    items[3].name = "VoiceNameLanguage";
    items[3].candidates = nameRvas;
    items[3].candidateCount = nameCount;
    items[3].expected = kExpectedVoiceName;
    items[3].mask = kMaskVoiceName;
    items[3].expectedSize = sizeof(kExpectedVoiceName);
    items[3].patch = patchName;
    items[3].patchSize = sizeof(patchName);
    items[3].allowFullScan = false;

    return ApplyPatchGroupAtomic(groupName, items, 4);
}

static bool ApplyPatchGroupAtomic(const char* groupName, PatchItem* items, int count)
{
    if (!items || count <= 0) return false;

    HMODULE mod = GetModuleHandleA(NULL);
    BYTE* base = (BYTE*)mod;

    // Phase 1: resolve and verify every expected byte pattern before writing anything.
    for (int i = 0; i < count; ++i) {
        if (items[i].candidates && items[i].candidateCount > 0 && items[i].mask) {
            items[i].target = ResolvePatchTargetCandidatesMasked(items[i].name,
                                                                 items[i].candidates,
                                                                 items[i].candidateCount,
                                                                 items[i].expected,
                                                                 items[i].mask,
                                                                 items[i].expectedSize,
                                                                 items[i].allowFullScan);
        } else {
            items[i].target = ResolvePatchTarget(items[i].name, items[i].rva,
                                                 items[i].expected, items[i].expectedSize);
        }
        items[i].oldProtect = 0;
        if (!items[i].target) {
            LogLine(LOG_PREFIX "%s: atomic group skipped because %s did not match", groupName, items[i].name);
            return false;
        }
    }

    // Phase 2: make every target writable before copying, so normal cases do not half-apply.
    for (int i = 0; i < count; ++i) {
        if (!VirtualProtect(items[i].target, items[i].patchSize, PAGE_EXECUTE_READWRITE, &items[i].oldProtect)) {
            LogLine(LOG_PREFIX "%s: atomic group skipped because VirtualProtect failed on %s, GetLastError=%lu",
                    groupName, items[i].name, GetLastError());
            for (int j = 0; j < i; ++j) {
                DWORD tmp = 0;
                VirtualProtect(items[j].target, items[j].patchSize, items[j].oldProtect, &tmp);
            }
            return false;
        }
    }

    // Phase 3: write all patches.
    for (int i = 0; i < count; ++i) {
        memcpy(items[i].target, items[i].patch, items[i].patchSize);
    }

    // Phase 4: restore protections and flush cache.
    for (int i = 0; i < count; ++i) {
        DWORD tmp = 0;
        VirtualProtect(items[i].target, items[i].patchSize, items[i].oldProtect, &tmp);
        FlushInstructionCache(GetCurrentProcess(), items[i].target, items[i].patchSize);
        LogLine(LOG_PREFIX "%s: patched at RVA 0x%08X", items[i].name, (unsigned)(items[i].target - base));
    }

    LogLine(LOG_PREFIX "%s: atomic group applied", groupName);
    return true;
}

// ---- Packed SteamStub runtime voice patching ----
static bool TryApplyPackedSteamVoicePatch(const char* reason, bool logMiss)
{
    if (!g_packedSteamRuntimeMode) return false;
    if (g_packedSteamVoiceApplied) return true;

    if (InterlockedCompareExchange(&g_voicePatchInProgress, 1, 0) != 0)
        return false;

    int voiceEnabled = g_voiceRuntimeEnabled;
    int assetPatch = ReadIniInt("Voice", "AssetPatch", 1) ? 1 : 0;
    int requireAssetPatch = ReadIniInt("Voice", "RequireAssetPatch", 1) ? 1 : 0;
    int voiceLang = ReadVoiceLanguage();

    bool ok = false;
    if (!voiceEnabled || !assetPatch || !requireAssetPatch) {
        if (logMiss)
            LogLine(LOG_PREFIX "Packed Steam voice runtime patch skipped by config: Enable=%d AssetPatch=%d RequireAssetPatch=%d",
                    voiceEnabled, assetPatch, requireAssetPatch);
    } else {
        if (logMiss)
            LogLine(LOG_PREFIX "Packed Steam voice runtime patch attempt: reason=%s", reason ? reason : "unknown");

        bool ready = VoiceFullPatchCandidateSetReady(
                kVoicePackageRvasSteamOnly, sizeof(kVoicePackageRvasSteamOnly) / sizeof(kVoicePackageRvasSteamOnly[0]),
                kVoiceResourceRvasSteamOnly, sizeof(kVoiceResourceRvasSteamOnly) / sizeof(kVoiceResourceRvasSteamOnly[0]),
                kVoiceBundleRvasSteamOnly, sizeof(kVoiceBundleRvasSteamOnly) / sizeof(kVoiceBundleRvasSteamOnly[0]),
                kVoiceNameRvasSteamOnly, sizeof(kVoiceNameRvasSteamOnly) / sizeof(kVoiceNameRvasSteamOnly[0]));

        if (!ready) {
            if (logMiss)
                LogLine(LOG_PREFIX "Packed Steam voice runtime patch targets are not ready yet: reason=%s",
                        reason ? reason : "unknown");
        } else {
            ok = ApplyVoiceFullPatchWithCandidateSets("VoiceFullPatch",
                    kVoicePackageRvasSteamOnly, sizeof(kVoicePackageRvasSteamOnly) / sizeof(kVoicePackageRvasSteamOnly[0]),
                    kVoiceResourceRvasSteamOnly, sizeof(kVoiceResourceRvasSteamOnly) / sizeof(kVoiceResourceRvasSteamOnly[0]),
                    kVoiceBundleRvasSteamOnly, sizeof(kVoiceBundleRvasSteamOnly) / sizeof(kVoiceBundleRvasSteamOnly[0]),
                    kVoiceNameRvasSteamOnly, sizeof(kVoiceNameRvasSteamOnly) / sizeof(kVoiceNameRvasSteamOnly[0]),
                    voiceLang, false);
        }

        if (ok) {
            InterlockedExchange(&g_packedSteamVoiceApplied, 1);
            InterlockedExchange(&g_voiceFullPatchApplied, 1);
            LogLine(LOG_PREFIX "Packed Steam voice runtime patch applied: language=%d reason=%s",
                    voiceLang, reason ? reason : "unknown");
            WriteCompatibilityReport();
        } else if (logMiss) {
            LogLine(LOG_PREFIX "Packed Steam voice runtime patch not ready yet: reason=%s",
                    reason ? reason : "unknown");
        }
    }

    InterlockedExchange(&g_voicePatchInProgress, 0);
    return ok;
}

static void TryPackedSteamVoicePatchFast(const char* reason)
{
    if (!g_packedSteamRuntimeMode || g_packedSteamVoiceApplied || g_voicePatchInProgress)
        return;
    // Silent fast path used by registry/file hooks. The thread emits progress logs.
    TryApplyPackedSteamVoicePatch(reason, false);
}

static DWORD WINAPI PackedSteamVoicePatchThreadProc(LPVOID)
{
    int tries = ClampInt(ReadIniInt("Debug", "PackedSteamPatchTries", 10000), 1, 60000);
    int interval = ClampInt(ReadIniInt("Debug", "PackedSteamPatchIntervalMs", 1), 1, 1000);
    int statusEvery = ClampInt(ReadIniInt("Debug", "PackedSteamPatchStatusEvery", 1000), 1, 60000);

    LogLine(LOG_PREFIX "Packed Steam runtime patch thread started: tries=%d interval=%dms", tries, interval);

    for (int i = 1; i <= tries; ++i) {
        if (TryApplyPackedSteamVoicePatch("runtime-thread", i == 1 || (i % statusEvery) == 0)) {
            LogLine(LOG_PREFIX "Packed Steam runtime patch thread complete after %d pass(es)", i);
            return 0;
        }
        Sleep((DWORD)interval);
    }

    LogLine(LOG_PREFIX "Packed Steam runtime patch thread ended without applying voice patch");
    return 0;
}

static void StartPackedSteamVoicePatchThreadIfNeeded()
{
    if (!g_packedSteamRuntimeMode || g_packedSteamVoiceApplied) return;
    if (InterlockedCompareExchange(&g_packedSteamVoiceThreadStarted, 1, 0) != 0) return;

    HANDLE h = CreateThread(NULL, 0, PackedSteamVoicePatchThreadProc, NULL, 0, NULL);
    if (h) CloseHandle(h);
    else {
        InterlockedExchange(&g_packedSteamVoiceThreadStarted, 0);
        LogLine(LOG_PREFIX "Packed Steam runtime patch thread CreateThread failed, GetLastError=%lu", GetLastError());
    }
}

// ---- DPI awareness ----
static void ApplyDpiAwareness()
{
    int enabled = ReadIniInt("DPI", "DPIAware", 1);
    int mode    = ClampInt(ReadIniInt("DPI", "DPIAwareness", 1), 1, 3);

    if (!enabled) {
        LogLine(LOG_PREFIX "DPI awareness disabled");
        return;
    }

    HMODULE user32 = GetModuleHandleA("user32.dll");
    if (!user32) user32 = LoadLibraryA("user32.dll");

    // Windows 10 1607+: SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)
    if (mode >= 3 && user32) {
        typedef BOOL (WINAPI *SetProcessDpiAwarenessContext_t)(HANDLE);
        SetProcessDpiAwarenessContext_t pSetProcessDpiAwarenessContext =
            (SetProcessDpiAwarenessContext_t)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
        if (pSetProcessDpiAwarenessContext) {
            if (pSetProcessDpiAwarenessContext((HANDLE)(LONG_PTR)-4)) {
                LogLine(LOG_PREFIX "DPI awareness active: Per-Monitor V2");
                return;
            }
            LogLine(LOG_PREFIX "Per-Monitor V2 DPI failed, GetLastError=%lu", GetLastError());
        }
    }

    // Windows 8.1+: SetProcessDpiAwareness(PROCESS_PER_MONITOR_DPI_AWARE)
    if (mode >= 2) {
        HMODULE shcore = GetModuleHandleA("shcore.dll");
        if (!shcore) shcore = LoadLibraryA("shcore.dll");
        if (shcore) {
            typedef HRESULT (WINAPI *SetProcessDpiAwareness_t)(int);
            SetProcessDpiAwareness_t pSetProcessDpiAwareness =
                (SetProcessDpiAwareness_t)GetProcAddress(shcore, "SetProcessDpiAwareness");
            if (pSetProcessDpiAwareness) {
                HRESULT hr = pSetProcessDpiAwareness(2); // PROCESS_PER_MONITOR_DPI_AWARE
                if (SUCCEEDED(hr)) {
                    LogLine(LOG_PREFIX "DPI awareness active: Per-Monitor");
                    return;
                }
                LogLine(LOG_PREFIX "Per-Monitor DPI failed, HRESULT=0x%08lX", (unsigned long)hr);
            }
        }
    }

    // Vista+: SetProcessDPIAware(), safest fallback.
    if (user32) {
        typedef BOOL (WINAPI *SetProcessDPIAware_t)(void);
        SetProcessDPIAware_t pSetProcessDPIAware =
            (SetProcessDPIAware_t)GetProcAddress(user32, "SetProcessDPIAware");
        if (pSetProcessDPIAware) {
            if (pSetProcessDPIAware()) {
                LogLine(LOG_PREFIX "DPI awareness active: System");
                return;
            }
            LogLine(LOG_PREFIX "System DPI failed, GetLastError=%lu", GetLastError());
        }
    }

    LogLine(LOG_PREFIX "DPI awareness could not be set");
}


// ---- CPU affinity limiter ----
static int CountBitsPtr(DWORD_PTR v)
{
    int c = 0;
    while (v) {
        c += (int)(v & 1);
        v >>= 1;
    }
    return c;
}

static DWORD_PTR BuildFirstNCoresMask(DWORD_PTR allowedMask, int maxCores)
{
    DWORD_PTR result = 0;
    int picked = 0;
    for (int bit = 0; bit < (int)(sizeof(DWORD_PTR) * 8); ++bit) {
        DWORD_PTR b = ((DWORD_PTR)1 << bit);
        if (allowedMask & b) {
            result |= b;
            ++picked;
            if (picked >= maxCores) break;
        }
    }
    return result;
}

static void ApplyCpuAffinityLimit()
{
    int enabled = ReadIniInt("Performance", "LimitCpuCores", 1) ? 1 : 0;
    int maxCores = ClampInt(ReadIniInt("Performance", "MaxCpuCores", 4), 1, 10);

    if (!enabled) {
        LogLine(LOG_PREFIX "CPU affinity limiter disabled");
        return;
    }

    HANDLE proc = GetCurrentProcess();
    DWORD_PTR processMask = 0;
    DWORD_PTR systemMask = 0;
    if (!GetProcessAffinityMask(proc, &processMask, &systemMask)) {
        LogLine(LOG_PREFIX "CPU affinity: GetProcessAffinityMask failed, GetLastError=%lu", GetLastError());
        return;
    }

    DWORD_PTR allowedMask = processMask ? processMask : systemMask;
    int available = CountBitsPtr(allowedMask);
    if (available <= 0) {
        LogLine(LOG_PREFIX "CPU affinity: no available CPU bits in affinity mask");
        return;
    }

    int targetCores = maxCores;
    if (targetCores > available) targetCores = available;

    DWORD_PTR newMask = BuildFirstNCoresMask(allowedMask, targetCores);
    if (!newMask) {
        LogLine(LOG_PREFIX "CPU affinity: computed mask is zero, skipped");
        return;
    }

    if (newMask == processMask) {
        g_cpuAffinityAppliedCores = targetCores;
        g_cpuAffinityMask = newMask;
        LogLine(LOG_PREFIX "CPU affinity already limited: max=%d available=%d mask=0x%p", maxCores, available, (void*)newMask);
        return;
    }

    if (SetProcessAffinityMask(proc, newMask)) {
        g_cpuAffinityAppliedCores = targetCores;
        g_cpuAffinityMask = newMask;
        LogLine(LOG_PREFIX "CPU affinity limited: requested=%d applied=%d available=%d mask=0x%p", maxCores, targetCores, available, (void*)newMask);
    } else {
        LogLine(LOG_PREFIX "CPU affinity: SetProcessAffinityMask failed, GetLastError=%lu requested=%d mask=0x%p", GetLastError(), maxCores, (void*)newMask);
    }
}

// ---- IAT hook installation ----
static void PatchIAT()
{
    HMODULE mod = GetModuleHandleA(NULL);
    if (!mod) return;

    BYTE* base = (BYTE*)mod;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;

    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;

    DWORD importRva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (!importRva) return;

    IMAGE_IMPORT_DESCRIPTOR* imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + importRva);
    int advapiHooks = 0;
    int kernelHooks = 0;
    int dinputHooks = 0;

    for (; imp->Name; ++imp) {
        char* dll = (char*)(base + imp->Name);
        bool isAdvapi = (lstrcmpiA(dll, "ADVAPI32.DLL") == 0);
        bool isKernel = (lstrcmpiA(dll, "KERNEL32.DLL") == 0 || lstrcmpiA(dll, "KERNELBASE.DLL") == 0);
        bool isDinput8 = (lstrcmpiA(dll, "DINPUT8.DLL") == 0);
        if (!isAdvapi && !isKernel && !isDinput8) continue;

        IMAGE_THUNK_DATA* originalThunk = imp->OriginalFirstThunk ?
            (IMAGE_THUNK_DATA*)(base + imp->OriginalFirstThunk) :
            (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
        IMAGE_THUNK_DATA* iatThunk = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
        if (!originalThunk || !iatThunk) continue;

        for (int i = 0; originalThunk[i].u1.AddressOfData; ++i) {
            if (originalThunk[i].u1.AddressOfData & IMAGE_ORDINAL_FLAG32) continue;

            IMAGE_IMPORT_BY_NAME* importByName = (IMAGE_IMPORT_BY_NAME*)(base + originalThunk[i].u1.AddressOfData);
            if (!importByName->Name) continue;

            const char* name = (const char*)importByName->Name;
            void** target = (void**)&iatThunk[i].u1.Function;
            void* replacement = NULL;
            void** realSlot = NULL;

            if (isAdvapi && lstrcmpiA(name, "RegQueryValueExA") == 0) {
                replacement = (void*)(DWORD_PTR)Hook_RegQueryValueExA;
                realSlot = (void**)&Real_RegQueryValueExA;
            } else if (isAdvapi && lstrcmpiA(name, "RegQueryValueExW") == 0) {
                replacement = (void*)(DWORD_PTR)Hook_RegQueryValueExW;
                realSlot = (void**)&Real_RegQueryValueExW;
            } else if (isKernel && lstrcmpiA(name, "CreateFileA") == 0) {
                replacement = (void*)(DWORD_PTR)Hook_CreateFileA;
                realSlot = (void**)&Real_CreateFileA;
            } else if (isKernel && lstrcmpiA(name, "CreateFileW") == 0) {
                replacement = (void*)(DWORD_PTR)Hook_CreateFileW;
                realSlot = (void**)&Real_CreateFileW;
            } else if (isKernel && lstrcmpiA(name, "GetProcAddress") == 0) {
                replacement = (void*)(DWORD_PTR)Hook_GetProcAddress;
                realSlot = (void**)&Real_GetProcAddress;
            } else if (isDinput8 && lstrcmpiA(name, "DirectInput8Create") == 0) {
                replacement = (void*)(DWORD_PTR)Hook_DirectInput8Create;
                realSlot = (void**)&Real_DirectInput8Create;
            }

            if (replacement && realSlot) {
                *realSlot = *target;
                DWORD old = 0;
                if (VirtualProtect(target, sizeof(void*), PAGE_READWRITE, &old)) {
                    *target = replacement;
                    VirtualProtect(target, sizeof(void*), old, &old);
                    if (isAdvapi) ++advapiHooks;
                    else if (isKernel) ++kernelHooks;
                    else if (isDinput8) ++dinputHooks;
                }
            }
        }
    }

    LogLine(LOG_PREFIX "ADVAPI32 registry hooks installed: %d", advapiHooks);
    LogLine(LOG_PREFIX "KERNEL32 hooks installed: %d", kernelHooks);
    LogLine(LOG_PREFIX "DINPUT8 input hooks installed: %d", dinputHooks);
    g_advapiHookCount = advapiHooks;
    g_kernelHookCount = kernelHooks;
    g_dinputHookCount = dinputHooks;
}

// ---- Window helpers ----
static int ReadWindowMode()
{
    // 0=fullscreen, 1=windowed, 2=borderless window. Default is intentionally windowed.
    return ClampInt(ReadIniInt("Display", "WindowMode", 2), 0, 2);
}

static BOOL CALLBACK EnumMainWindowProc(HWND hwnd, LPARAM lParam)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId()) return TRUE;
    if (!IsWindowVisible(hwnd)) return TRUE;
    if (GetWindow(hwnd, GW_OWNER) != NULL) return TRUE;

    *(HWND*)lParam = hwnd;
    return FALSE;
}

static HWND FindMainGameWindow()
{
    HWND hwnd = NULL;
    EnumWindows(EnumMainWindowProc, (LPARAM)&hwnd);
    return hwnd;
}

static void ApplyBorderlessToWindow(HWND hwnd)
{
    if (!hwnd) return;

    LONG_PTR style = GetWindowLongPtrA(hwnd, GWL_STYLE);
    LONG_PTR exStyle = GetWindowLongPtrA(hwnd, GWL_EXSTYLE);

    style &= ~(WS_CAPTION | WS_THICKFRAME | WS_BORDER | WS_DLGFRAME);
    exStyle &= ~(WS_EX_DLGMODALFRAME | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE);

    SetWindowLongPtrA(hwnd, GWL_STYLE, style);
    SetWindowLongPtrA(hwnd, GWL_EXSTYLE, exStyle);

    int desiredW = ClampInt(ReadIniInt("Display", "Width", 1920), 320, 16384);
    int desiredH = ClampInt(ReadIniInt("Display", "Height", 1080), 240, 16384);
    int center = ReadIniInt("Display", "BorderlessCenter", 0) ? 1 : 0;

    RECT curRc;
    GetWindowRect(hwnd, &curRc);

    int x = curRc.left;
    int y = curRc.top;

    if (center) {
        HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi;
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoA(mon, &mi)) {
            RECT rc = mi.rcWork;
            int monW = rc.right - rc.left;
            int monH = rc.bottom - rc.top;
            x = rc.left + (monW - desiredW) / 2;
            y = rc.top + (monH - desiredH) / 2;
        } else {
            LogLine(LOG_PREFIX "Borderless: GetMonitorInfo failed, GetLastError=%lu", GetLastError());
        }
    }

    SetWindowPos(hwnd, HWND_TOP,
                 x, y, desiredW, desiredH,
                 SWP_NOOWNERZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);

    LogLine(LOG_PREFIX "Borderless window applied: %d,%d %dx%d center=%d",
            x, y, desiredW, desiredH, center);
}

static DWORD WINAPI BorderlessThreadProc(LPVOID)
{
    // Wait for the game's main window. This avoids doing window manipulation from DllMain.
    for (int i = 0; i < 600; ++i) { // up to about 60 seconds
        if (ReadWindowMode() != 2) return 0;
        HWND hwnd = FindMainGameWindow();
        if (hwnd) {
            ApplyBorderlessToWindow(hwnd);
            return 0;
        }
        Sleep(100);
    }
    LogLine(LOG_PREFIX "Borderless: main window not found within timeout");
    return 0;
}

static void StartBorderlessThreadIfNeeded()
{
    if (ReadWindowMode() != 2) return;
    HANDLE h = CreateThread(NULL, 0, BorderlessThreadProc, NULL, 0, NULL);
    if (h) CloseHandle(h);
    else LogLine(LOG_PREFIX "Borderless: CreateThread failed, GetLastError=%lu", GetLastError());
}

// ---- Focus-loss mouse release ----
static DWORD WINAPI FocusLossMouseReleaseThreadProc(LPVOID)
{
    bool wasUnfocused = false;
    for (;;) {
        if (!ShouldReleaseMouseOnFocusLoss()) return 0;

        HWND foreground = GetForegroundWindow();
        DWORD foregroundPid = 0;
        if (foreground) GetWindowThreadProcessId(foreground, &foregroundPid);
        bool unfocused = (foregroundPid != GetCurrentProcessId());

        if (unfocused) {
            // Repeat while unfocused because the game may try to re-clip the cursor.
            ClipCursor(NULL);
            if (!wasUnfocused)
                LogTrace(LOG_PREFIX "Mouse released after game focus loss");
        }
        wasUnfocused = unfocused;
        Sleep(50);
    }
}

static void StartFocusLossMouseReleaseThreadIfNeeded()
{
    if (!ShouldReleaseMouseOnFocusLoss()) return;
    if (InterlockedCompareExchange(&g_focusReleaseThreadStarted, 1, 0) != 0) return;
    HANDLE h = CreateThread(NULL, 0, FocusLossMouseReleaseThreadProc, NULL, 0, NULL);
    if (h) {
        CloseHandle(h);
        LogLine(LOG_PREFIX "Focus-loss mouse release enabled for windowed/borderless mode");
    } else {
        InterlockedExchange(&g_focusReleaseThreadStarted, 0);
        LogWarn(LOG_PREFIX "Focus-loss mouse release thread failed, GetLastError=%lu", GetLastError());
    }
}

// ---- Runtime memory patches ----
static void PatchGameMemory()
{
    int textLang = ReadTextLanguage();
    int voiceLang = ReadVoiceLanguage();
    int voiceEnabled = g_voiceRuntimeEnabled;
    int assetPatch = ReadIniInt("Voice", "AssetPatch", 1) ? 1 : 0;
    int requireAssetPatch = ReadIniInt("Voice", "RequireAssetPatch", 1) ? 1 : 0;

    LogExecutableFlavor();
    LogLine(LOG_PREFIX "TextLanguage=%d active via LNG_Language registry hook", textLang);
    LogLine(LOG_PREFIX "VoiceLanguage=%d", voiceLang);

    if (!voiceEnabled) {
        LogWarn(LOG_PREFIX "Voice patch disabled by config or because no requested/fallback voice package was found");
    } else if (IsPackedSteamStub()) {
        LogLine(LOG_PREFIX "Packed SteamStub detected: voice patch will wait for runtime unpacking and fixed Steam RVAs");
        InterlockedExchange(&g_packedSteamRuntimeMode, 1);
        TryApplyPackedSteamVoicePatch("startup", true);
        StartPackedSteamVoicePatchThreadIfNeeded();
    } else if (assetPatch && requireAssetPatch) {
        LogLine(LOG_PREFIX "Voice FullPatch=1: Package + Resource + Bundle + Name will be patched transactionally");
        if (ApplyVoiceFullPatchWithCandidateSets("VoiceFullPatch",
                kVoicePackageRvasAll, sizeof(kVoicePackageRvasAll) / sizeof(kVoicePackageRvasAll[0]),
                kVoiceResourceRvasAll, sizeof(kVoiceResourceRvasAll) / sizeof(kVoiceResourceRvasAll[0]),
                kVoiceBundleRvasAll, sizeof(kVoiceBundleRvasAll) / sizeof(kVoiceBundleRvasAll[0]),
                kVoiceNameRvasAll, sizeof(kVoiceNameRvasAll) / sizeof(kVoiceNameRvasAll[0]),
                voiceLang, false)) {
            InterlockedExchange(&g_voiceFullPatchApplied, 1);
            LogLine(LOG_PREFIX "Voice package language=%d active", voiceLang);
            LogLine(LOG_PREFIX "Voice asset language=%d active", voiceLang);
        } else {
            LogLine(LOG_PREFIX "VoiceFullPatch failed; no partial voice patch was applied");
        }
    } else {
        LogLine(LOG_PREFIX "Voice patch warning: RequireAssetPatch=0 or AssetPatch=0 allows partial/legacy behavior");

        BYTE patchPackage[13];
        BYTE patchResource[5];
        BYTE patchBundle[5];
        BYTE patchName[5];
        BuildVoicePatchBytes(voiceLang, patchPackage, patchResource, patchBundle, patchName);

        if (PatchByCandidatesMasked("VoicePackageLanguage",
                                   kVoicePackageRvasAll, sizeof(kVoicePackageRvasAll) / sizeof(kVoicePackageRvasAll[0]),
                                   kExpectedVoicePackage, kMaskVoicePackage, sizeof(kExpectedVoicePackage),
                                   patchPackage, sizeof(patchPackage), false)) {
            LogLine(LOG_PREFIX "Voice package language=%d active", voiceLang);
        }

        if (assetPatch) {
            PatchItem items[3];
            ZeroMemory(items, sizeof(items));

            items[0].name = "VoiceResourceLanguage";
            items[0].candidates = kVoiceResourceRvasAll;
            items[0].candidateCount = sizeof(kVoiceResourceRvasAll) / sizeof(kVoiceResourceRvasAll[0]);
            items[0].expected = kExpectedVoiceResource;
            items[0].mask = kMaskVoiceResource;
            items[0].expectedSize = sizeof(kExpectedVoiceResource);
            items[0].patch = patchResource;
            items[0].patchSize = sizeof(patchResource);
            items[0].allowFullScan = false;

            items[1].name = "VoiceBundleLanguage";
            items[1].candidates = kVoiceBundleRvasAll;
            items[1].candidateCount = sizeof(kVoiceBundleRvasAll) / sizeof(kVoiceBundleRvasAll[0]);
            items[1].expected = kExpectedVoiceBundle;
            items[1].mask = kMaskVoiceBundle;
            items[1].expectedSize = sizeof(kExpectedVoiceBundle);
            items[1].patch = patchBundle;
            items[1].patchSize = sizeof(patchBundle);
            items[1].allowFullScan = false;

            items[2].name = "VoiceNameLanguage";
            items[2].candidates = kVoiceNameRvasAll;
            items[2].candidateCount = sizeof(kVoiceNameRvasAll) / sizeof(kVoiceNameRvasAll[0]);
            items[2].expected = kExpectedVoiceName;
            items[2].mask = kMaskVoiceName;
            items[2].expectedSize = sizeof(kExpectedVoiceName);
            items[2].patch = patchName;
            items[2].patchSize = sizeof(patchName);
            items[2].allowFullScan = false;

            if (ApplyPatchGroupAtomic("VoiceAssetPatch", items, 3)) {
                LogLine(LOG_PREFIX "Voice asset language=%d active", voiceLang);
            } else {
                LogLine(LOG_PREFIX "VoiceAssetPatch failed as an atomic group; no partial asset patch was applied");
            }
        } else {
            LogLine(LOG_PREFIX "Voice AssetPatch disabled by [Voice] AssetPatch=0");
        }
    }

    // Display mode patch. WindowMode=1 and 2 both require the game to start as a window.
    int windowMode = ReadWindowMode();
    if (windowMode >= 1) {
        static const BYTE expectedFullScreen[20] = {
            '/', 'f','u','l','l','S','c','r','e','e','n',':','o','n',
            0,0,0,0,0,0
        };
        static const BYTE patchWindowed[20] = {
            '/', 'f','u','l','l','S','c','r','e','e','n',':','o','f','f',
            0,0,0,0,0
        };
        if (PatchByRVAOrScan("WindowMode", 0x00929D9C,
                             expectedFullScreen, sizeof(expectedFullScreen),
                             patchWindowed, sizeof(patchWindowed))) {
            InterlockedExchange(&g_windowPatchApplied, 1);
            LogLine(LOG_PREFIX "WindowMode=%d active", windowMode);
        }
    } else {
        LogLine(LOG_PREFIX "WindowMode=0: fullscreen requested; window patch disabled");
    }
}

// ---- Configuration validation and default file ----
static int NormalizeBool(int v) { return v ? 1 : 0; }

static int NearestAntiAliasing(int value)
{
    const int allowed[] = {0, 2, 4, 8};
    int best = allowed[0];
    int bestDist = abs(value - best);
    for (int i = 1; i < 4; ++i) {
        int d = abs(value - allowed[i]);
        if (d < bestDist) { bestDist = d; best = allowed[i]; }
    }
    return best;
}

static void ValidateOneRange(const char* section, const char* key, int def, int lo, int hi)
{
    int value = ReadIniInt(section, key, def);
    int fixed = ClampInt(value, lo, hi);
    if (value != fixed) {
        WriteIniInt(section, key, fixed);
        ++g_configCorrectionCount;
        LogWarn(LOG_PREFIX "Config corrected: [%s] %s=%d -> %d", section, key, value, fixed);
    }
}

static void ValidateOneBool(const char* section, const char* key, int def)
{
    int value = ReadIniInt(section, key, def);
    int fixed = NormalizeBool(value);
    if (value != fixed) {
        WriteIniInt(section, key, fixed);
        ++g_configCorrectionCount;
        LogWarn(LOG_PREFIX "Config corrected: [%s] %s=%d -> %d", section, key, value, fixed);
    }
}

static void ValidateAndCorrectConfig()
{
    // LogLevel is validated first so all later corrections can be logged.
    int rawLogLevel = ReadIniInt("Debug", "LogLevel", 2);
    g_logLevel = ClampInt(rawLogLevel, 0, 3);
    if (rawLogLevel != g_logLevel) {
        WriteIniInt("Debug", "LogLevel", g_logLevel);
        ++g_configCorrectionCount;
    }

    ValidateOneRange("Language", "TextLanguage", 6, 0, 13);
    ValidateOneRange("Language", "VoiceLanguage", 1, 1, 13);
    ValidateOneBool("Voice", "Enable", 1);
    ValidateOneBool("Voice", "AssetPatch", 1);
    ValidateOneBool("Voice", "RequireAssetPatch", 1);
    ValidateOneBool("Voice", "AutoFallback", 1);
    ValidateOneRange("Voice", "FallbackLanguage", 1, 1, 13);

    ValidateOneRange("Display", "WindowMode", 2, 0, 2);
    ValidateOneBool("Display", "BorderlessCenter", 0);
    ValidateOneRange("Display", "Width", 1920, 320, 16384);
    ValidateOneRange("Display", "Height", 1080, 240, 16384);
    ValidateOneBool("Display", "VSync", 1);
    ValidateOneBool("Display", "Widescreen", 1);
    int aspect = ReadIniInt("Display", "AspectRatio", 0);
    int fixedAspect = aspect <= 0 ? 0 : ClampInt(aspect, 50, 500);
    if (aspect != fixedAspect) {
        WriteIniInt("Display", "AspectRatio", fixedAspect);
        ++g_configCorrectionCount;
        LogWarn(LOG_PREFIX "Config corrected: [Display] AspectRatio=%d -> %d", aspect, fixedAspect);
    }

    ValidateOneBool("DPI", "DPIAware", 1);
    ValidateOneRange("DPI", "DPIAwareness", 1, 1, 3);
    ValidateOneBool("Performance", "LimitCpuCores", 1);
    ValidateOneRange("Performance", "MaxCpuCores", 4, 1, 10);
    ValidateOneBool("Input", "AllowWinKeyInWindowed", 1);
    ValidateOneBool("Input", "ReleaseMouseOnFocusLoss", 1);

    ValidateOneRange("Graphics", "Quality", 2, 0, 2);
    int aa = ReadIniInt("Graphics", "AntiAliasing", 4);
    int fixedAa = NearestAntiAliasing(aa);
    if (aa != fixedAa) {
        WriteIniInt("Graphics", "AntiAliasing", fixedAa);
        ++g_configCorrectionCount;
        LogWarn(LOG_PREFIX "Config corrected: [Graphics] AntiAliasing=%d -> %d", aa, fixedAa);
    }
    ValidateOneBool("Graphics", "HighResolutionTextures", 1);
    ValidateOneRange("AdvancedGraphics", "ShadowQuality", 2, 0, 2);
    ValidateOneRange("AdvancedGraphics", "PostEffects", 2, 0, 2);

    ValidateOneBool("Debug", "CompatibilityReport", 1);
    ValidateOneRange("Debug", "PackedSteamPatchTries", 10000, 1, 60000);
    ValidateOneRange("Debug", "PackedSteamPatchIntervalMs", 1, 1, 1000);
    ValidateOneRange("Debug", "PackedSteamPatchStatusEvery", 1000, 1, 60000);
}

static void CreateDefaultIni()
{
    if (GetFileAttributesW(g_iniPath) != INVALID_FILE_ATTRIBUTES) return;

    const wchar_t* ini =
        L"; ============================================================\r\n"
        L"; PoP4 Universal Patch v28 stable configuration\r\n"
        L"; This file is created as UTF-16 so non-ASCII paths and comments are safe.\r\n"
        L"; Put it next to PoP_UniversalPatch.asi / PoP_Settings.asi.\r\n"
        L"; The plugin never edits the game EXE; all patches are runtime-only.\r\n"
        L"; GOG, Steam unpacked and original packed SteamStub EXEs are auto-detected.\r\n"
        L"; ============================================================\r\n\r\n"

        L"; -------------------- Language --------------------\r\n"
        L"[Language]\r\n"
        L"; Text/UI/subtitle language. The ASI returns this value when the game asks\r\n"
        L"; for the original registry key LNG_Language.\r\n"
        L"; 0=None/Auto, 1=English, 2=French, 3=Spanish, 4=Polish, 5=German,\r\n"
        L"; 6=Chinese, 7=Hungarian, 8=Italian, 9=Japanese, 10=Czech,\r\n"
        L"; 11=Korean, 12=Russian, 13=Dutch.\r\n"
        L"TextLanguage=6\r\n\r\n"
        L"; Requested dialogue voice language, using the same ID table.\r\n"
        L"; A matching DataPC_StreamedSoundsXXX.forge must exist.\r\n"
        L"VoiceLanguage=1\r\n\r\n"

        L"; -------------------- Voice --------------------\r\n"
        L"[Voice]\r\n"
        L"; 1 enables independent text and voice languages; 0 leaves game voice logic untouched.\r\n"
        L"Enable=1\r\n\r\n"
        L"; Keep both options at 1. Package + Resource + Bundle + Name are then applied\r\n"
        L"; as one transaction, preventing unsafe half-patched language states.\r\n"
        L"AssetPatch=1\r\n"
        L"RequireAssetPatch=1\r\n\r\n"
        L"; AutoFallback checks whether the requested voice forge exists before patching.\r\n"
        L"; If it is missing, the ASI safely uses FallbackLanguage. If both are missing,\r\n"
        L"; voice patches are disabled instead of forcing a broken package selection.\r\n"
        L"AutoFallback=1\r\n"
        L"FallbackLanguage=1\r\n\r\n"

        L"; -------------------- Display --------------------\r\n"
        L"[Display]\r\n"
        L"; 0=fullscreen, 1=windowed (default), 2=borderless window at Width x Height.\r\n"
        L"WindowMode=2\r\n"
        L"; Only affects mode 2: 0=keep game position, 1=center the window.\r\n"
        L"BorderlessCenter=0\r\n"
        L"Width=1920\r\n"
        L"Height=1080\r\n"
        L"; 0=off, 1=on.\r\n"
        L"VSync=1\r\n"
        L"Widescreen=1\r\n"
        L"; 0=auto floor((Width/Height)*100); otherwise use a manual value, e.g. 177.\r\n"
        L"AspectRatio=0\r\n\r\n"

        L"; -------------------- DPI --------------------\r\n"
        L"[DPI]\r\n"
        L"DPIAware=1\r\n"
        L"; 1=System DPI (recommended), 2=Per-monitor, 3=Per-monitor V2.\r\n"
        L"DPIAwareness=1\r\n\r\n"

        L"; -------------------- Performance --------------------\r\n"
        L"[Performance]\r\n"
        L"; Limits this old game to a smaller logical-CPU set.\r\n"
        L"LimitCpuCores=1\r\n"
        L"; Automatically clamped to 1..10; default 4.\r\n"
        L"MaxCpuCores=4\r\n\r\n"

        L"; -------------------- Input --------------------\r\n"
        L"[Input]\r\n"
        L"; These options only apply to WindowMode 1 or 2; fullscreen is untouched.\r\n"
        L"; Clears DirectInput DISCL_NOWINKEY so the Windows key remains usable.\r\n"
        L"AllowWinKeyInWindowed=1\r\n"
        L"; Releases ClipCursor whenever the game loses focus. It does not permanently\r\n"
        L"; disable normal mouse capture while the game is focused.\r\n"
        L"ReleaseMouseOnFocusLoss=1\r\n\r\n"

        L"; -------------------- Graphics --------------------\r\n"
        L"[Graphics]\r\n"
        L"; 0=Low, 1=Medium, 2=High.\r\n"
        L"Quality=2\r\n"
        L"; Valid values: 0=off, 2=x2, 4=x4, 8=x8. Invalid values are corrected.\r\n"
        L"AntiAliasing=4\r\n"
        L"; 1=high-resolution textures, 0=degraded/low textures.\r\n"
        L"HighResolutionTextures=1\r\n\r\n"

        L"[AdvancedGraphics]\r\n"
        L"; Extra engine registry values; leave at defaults unless testing.\r\n"
        L"ShadowQuality=2\r\n"
        L"PostEffects=2\r\n\r\n"

        L"; -------------------- Debug / diagnostics --------------------\r\n"
        L"[Debug]\r\n"
        L"; LogLevel combines the old log switch and tracing controls:\r\n"
        L";   0 = completely disable the log file\r\n"
        L";   1 = warnings/errors and config corrections only\r\n"
        L";   2 = normal startup/patch information (recommended)\r\n"
        L";   3 = verbose, including DataPC/forge file-open tracing\r\n"
        L"LogLevel=2\r\n"
        L"; Writes PoP_CompatibilityReport.txt with EXE, hooks, voice packs and settings.\r\n"
        L"CompatibilityReport=1\r\n"
        L"; Packed-Steam runtime patch controls. Normal users should keep defaults.\r\n"
        L"PackedSteamPatchTries=10000\r\n"
        L"PackedSteamPatchIntervalMs=1\r\n"
        L"PackedSteamPatchStatusEvery=1000\r\n";

    HANDLE f = CreateFileW(g_iniPath, GENERIC_WRITE, 0, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD w = 0;
        const WORD bom = 0xFEFF;
        WriteFile(f, &bom, sizeof(bom), &w, NULL);
        WriteFile(f, ini, (DWORD)(lstrlenW(ini) * sizeof(wchar_t)), &w, NULL);
        CloseHandle(f);
    }
}

static void GetModuleDirW(HMODULE module, wchar_t* out, DWORD count)
{
    if (!out || count == 0) return;
    out[0] = 0;
    GetModuleFileNameW(module, out, count);
    out[count - 1] = 0;
    wchar_t* slash = wcsrchr(out, L'\\');
    if (slash) *slash = 0;
}

static const wchar_t* ExeFlavorNameW()
{
    if (g_exeFlavor == 3) return L"Steam packed / SteamStub";
    if (g_exeFlavor == 2) return L"Steam unpacked";
    if (g_exeFlavor == 1) return L"GOG / unpacked-like";
    return L"Unknown / not evaluated";
}

static void ReportLine(HANDLE f, const wchar_t* fmt, ...)
{
    if (f == INVALID_HANDLE_VALUE) return;
    wchar_t line[2048];
    va_list ap;
    va_start(ap, fmt);
#if defined(_MSC_VER)
    _vsnwprintf_s(line, 2048, _TRUNCATE, fmt, ap);
#else
    vswprintf(line, 2048, fmt, ap);
#endif
    va_end(ap);
    line[2047] = 0;
    DWORD w = 0;
    WriteFile(f, line, (DWORD)(lstrlenW(line) * sizeof(wchar_t)), &w, NULL);
    WriteFile(f, L"\r\n", 4, &w, NULL);
}

static void WriteCompatibilityReport()
{
    if (!ReadIniInt("Debug", "CompatibilityReport", 1)) return;

    HANDLE f = CreateFileW(g_reportPath, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        LogWarn(LOG_PREFIX "Compatibility report could not be created, GetLastError=%lu", GetLastError());
        return;
    }

    DWORD w = 0;
    const WORD bom = 0xFEFF;
    WriteFile(f, &bom, sizeof(bom), &w, NULL);

    wchar_t exePath[MAX_PATH * 4] = {0};
    GetModuleFileNameW(NULL, exePath, MAX_PATH * 4);
    WIN32_FILE_ATTRIBUTE_DATA fad;
    ZeroMemory(&fad, sizeof(fad));
    GetFileAttributesExW(exePath, GetFileExInfoStandard, &fad);
    ULONGLONG exeSize = ((ULONGLONG)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;

    ReportLine(f, L"PoP4 Universal Patch v28 compatibility report");
    ReportLine(f, L"============================================================");
    ReportLine(f, L"Game EXE: %s", exePath);
    ReportLine(f, L"EXE flavor: %s", ExeFlavorNameW());
    ReportLine(f, L"Entry RVA: 0x%08X", GetEntryPointRva());
    ReportLine(f, L"EXE size: %llu bytes", (unsigned long long)exeSize);
    ReportLine(f, L"ASI directory: %s", g_asiDir);
    ReportLine(f, L"Config path: %s", g_iniPath);
    ReportLine(f, L"");

    ReportLine(f, L"Configuration");
    ReportLine(f, L"-------------");
    ReportLine(f, L"Corrections applied this run: %d", g_configCorrectionCount);
    ReportLine(f, L"LogLevel: %d", g_logLevel);
    ReportLine(f, L"TextLanguage: %d (%s)", ReadTextLanguage(), VoiceLanguageNameW(ReadTextLanguage()));
    ReportLine(f, L"Requested VoiceLanguage: %d (%s)", ReadConfiguredVoiceLanguage(), VoiceLanguageNameW(ReadConfiguredVoiceLanguage()));
    ReportLine(f, L"Effective VoiceLanguage: %d (%s)", g_effectiveVoiceLanguage, VoiceLanguageNameW(g_effectiveVoiceLanguage));
    ReportLine(f, L"Voice runtime enabled: %d", g_voiceRuntimeEnabled);
    ReportLine(f, L"WindowMode: %d", ReadWindowMode());
    ReportLine(f, L"Resolution: %dx%d", ReadIniInt("Display", "Width", 1920), ReadIniInt("Display", "Height", 1080));
    ReportLine(f, L"AspectRatioOverride: %u", (unsigned)ReadAspectRatioOverride());
    ReportLine(f, L"CPU affinity cores applied: %d", g_cpuAffinityAppliedCores);
    ReportLine(f, L"CPU affinity mask: 0x%p", (void*)g_cpuAffinityMask);
    ReportLine(f, L"");

    ReportLine(f, L"Patch and hook state");
    ReportLine(f, L"--------------------");
    ReportLine(f, L"Voice full patch applied: %d", (int)g_voiceFullPatchApplied);
    ReportLine(f, L"Window patch applied: %d", (int)g_windowPatchApplied);
    ReportLine(f, L"Packed Steam runtime mode: %d", (int)g_packedSteamRuntimeMode);
    ReportLine(f, L"Packed Steam voice applied: %d", (int)g_packedSteamVoiceApplied);
    ReportLine(f, L"ADVAPI32 hooks: %d", g_advapiHookCount);
    ReportLine(f, L"KERNEL32 hooks: %d", g_kernelHookCount);
    ReportLine(f, L"DINPUT8 import hooks: %d", g_dinputHookCount);
    ReportLine(f, L"DirectInput CreateDevice vtable hooked: %d", (int)g_diCreateDeviceHooked);
    ReportLine(f, L"DirectInput SetCooperativeLevel vtable hooked: %d", (int)g_diSetCoopHooked);
    ReportLine(f, L"");

    ReportLine(f, L"Detected voice packages");
    ReportLine(f, L"-----------------------");
    for (int id = 1; id <= 13; ++id) {
        wchar_t path[MAX_PATH * 4] = {0};
        BuildVoicePackPathW(id, path, MAX_PATH * 4);
        ReportLine(f, L"%2d %-12s : %s", id, VoiceLanguageNameW(id), VoicePackExists(id) ? L"FOUND" : L"missing");
    }

    CloseHandle(f);
    LogLine(LOG_PREFIX "Compatibility report written");
}

static DWORD WINAPI CompatibilityReportThreadProc(LPVOID)
{
    Sleep(2000);
    WriteCompatibilityReport();
    return 0;
}

static void StartCompatibilityReportThreadIfNeeded()
{
    if (!ReadIniInt("Debug", "CompatibilityReport", 1)) return;
    if (InterlockedCompareExchange(&g_reportThreadStarted, 1, 0) != 0) return;
    HANDLE h = CreateThread(NULL, 0, CompatibilityReportThreadProc, NULL, 0, NULL);
    if (h) CloseHandle(h);
    else InterlockedExchange(&g_reportThreadStarted, 0);
}

BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hInst);

        GetModuleDirW(hInst, g_asiDir, MAX_PATH * 4);
        GetModuleDirW(NULL, g_gameDir, MAX_PATH * 4);
#if defined(_MSC_VER)
        _snwprintf_s(g_iniPath, MAX_PATH * 4, _TRUNCATE, L"%ls\\PoP_UniversalPatch.ini", g_asiDir);
        _snwprintf_s(g_logPath, MAX_PATH * 4, _TRUNCATE, L"%ls\\PoP_UniversalPatch.log", g_asiDir);
        _snwprintf_s(g_reportPath, MAX_PATH * 4, _TRUNCATE, L"%ls\\PoP_CompatibilityReport.txt", g_asiDir);
#else
        swprintf(g_iniPath, MAX_PATH * 4, L"%ls\\PoP_UniversalPatch.ini", g_asiDir);
        swprintf(g_logPath, MAX_PATH * 4, L"%ls\\PoP_UniversalPatch.log", g_asiDir);
        swprintf(g_reportPath, MAX_PATH * 4, L"%ls\\PoP_CompatibilityReport.txt", g_asiDir);
#endif

        CreateDefaultIni();
        ValidateAndCorrectConfig();

        DeleteFileW(g_logPath); // Start each run with a fresh log; level 0 leaves no file.
        if (g_logLevel > 0) {
            LogLine(LOG_PREFIX "loaded");
            char iniUtf8[MAX_PATH * 4] = {0};
            WideToUtf8(g_iniPath, iniUtf8, sizeof(iniUtf8));
            LogLine(LOG_PREFIX "ini=%s", iniUtf8);
        }

        ResolveVoiceLanguageAndFallback();
        ApplyDpiAwareness();
        ApplyCpuAffinityLimit();
        PatchIAT();
        PatchGameMemory();
        StartBorderlessThreadIfNeeded();
        StartFocusLossMouseReleaseThreadIfNeeded();
        StartCompatibilityReportThreadIfNeeded();
    }
    return TRUE;
}
