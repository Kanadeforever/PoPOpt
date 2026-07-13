#include "SettingsRegistryModule.h"

namespace popopt::settings {

using RegQueryValueExA_t = LONG (WINAPI *)(HKEY, LPCSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
using RegQueryValueExW_t = LONG (WINAPI *)(HKEY, LPCWSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);

static RegQueryValueExA_t g_realA = nullptr;
static RegQueryValueExW_t g_realW = nullptr;
static volatile LONG g_reentry = 0;
static volatile LONG g_hooksRegistered = 0;
static volatile LONG g_languageQueryCount = 0;
static volatile LONG g_lastLanguageValue = -1;

struct RegistryEntry {
    const char* regName;
    const char* section;
    const char* key;
    int defaultValue;
};

static const RegistryEntry kEntries[] = {
    {"LNG_Language",           "Language",         "TextLanguage",          6},
    {"ScreenResolutionWidth",  "Display",          "Width",              1920},
    {"ScreenResolutionHeight", "Display",          "Height",             1080},
    {"VerticalSync",           "Display",          "VSync",                 1},
    {"Antialiasing",           "Graphics",         "AntiAliasing",          4},
    {"Shadows",                "AdvancedGraphics", "ShadowQuality",         2},
    {"PostEffects",            "AdvancedGraphics", "PostEffects",           2},
    {"VisualQualityLvl",       "Graphics",         "Quality",               2},
};

int ReadTextLanguage()
{
    return ClampInt(Ini().GetInt("Language", "TextLanguage", 6), 0, 13);
}

DWORD ReadAspectRatioOverride()
{
    if (!Ini().GetInt("Display", "Widescreen", 1)) return 0;
    int aspect = Ini().GetInt("Display", "AspectRatio", 0);
    if (aspect <= 0) {
        int width = Ini().GetInt("Display", "Width", 1920);
        int height = Ini().GetInt("Display", "Height", 1080);
        aspect = (width > 0 && height > 0) ? (width * 100) / height : 177;
    }
    return (DWORD)ClampInt(aspect, 0, 999);
}

static DWORD ReadDegradedTextures()
{
    return Ini().GetInt("Graphics", "HighResolutionTextures", 1) ? 0u : 1u;
}

static DWORD ReadLauncherTextureLevel()
{
    return Ini().GetInt("Graphics", "HighResolutionTextures", 1) ? 1u : 0u;
}

static bool LookupValueA(LPCSTR name, DWORD* outValue)
{
    if (!name || !outValue) return false;
    if (_stricmp(name, "AspectRatioOverride") == 0) {
        *outValue = ReadAspectRatioOverride();
        return true;
    }
    if (_stricmp(name, "DegradedTextures") == 0) {
        *outValue = ReadDegradedTextures();
        return true;
    }
    if (_stricmp(name, "Texture_lvl") == 0) {
        *outValue = ReadLauncherTextureLevel();
        return true;
    }
    if (_stricmp(name, "Widescreen") == 0 ||
        _stricmp(name, "WideScreen") == 0 ||
        _stricmp(name, "Platform_PCWidescreen") == 0) {
        *outValue = Ini().GetInt("Display", "Widescreen", 1) ? 1u : 0u;
        return true;
    }

    for (const RegistryEntry& entry : kEntries) {
        if (_stricmp(name, entry.regName) != 0) continue;
        int value = Ini().GetInt(entry.section, entry.key, entry.defaultValue);
        if (_stricmp(entry.regName, "LNG_Language") == 0) value = ReadTextLanguage();
        if (value == -1) return false;
        *outValue = (DWORD)value;
        return true;
    }
    return false;
}

static LONG WINAPI HookA(HKEY key, LPCSTR valueName, LPDWORD reserved,
                         LPDWORD type, LPBYTE data, LPDWORD dataSize)
{
    if (InterlockedExchange(&g_reentry, 1)) {
        return g_realA ? g_realA(key, valueName, reserved, type, data, dataSize)
                       : ERROR_FILE_NOT_FOUND;
    }

    const bool languageQuery = valueName && _stricmp(valueName, "LNG_Language") == 0;
    if (languageQuery)
        Events().NotifyLanguageRegistryQuery("RegQueryValueExA(LNG_Language)");

    DWORD value = 0;
    LONG result = ERROR_SUCCESS;
    if (LookupValueA(valueName, &value)) {
        if (type) *type = REG_DWORD;
        if (data && dataSize && *dataSize >= sizeof(DWORD)) *(DWORD*)data = value;
        if (dataSize) *dataSize = sizeof(DWORD);

        if (languageQuery) {
            InterlockedExchange(&g_lastLanguageValue, (LONG)value);
            const LONG count = InterlockedIncrement(&g_languageQueryCount);
            if (count == 1)
                Log().Write(2, "LNG_Language intercepted through RegQueryValueExA: returned=%lu",
                            (unsigned long)value);
        }
    } else {
        result = g_realA ? g_realA(key, valueName, reserved, type, data, dataSize)
                         : ERROR_FILE_NOT_FOUND;
    }
    InterlockedExchange(&g_reentry, 0);
    return result;
}

static LONG WINAPI HookW(HKEY key, LPCWSTR valueName, LPDWORD reserved,
                         LPDWORD type, LPBYTE data, LPDWORD dataSize)
{
    if (InterlockedExchange(&g_reentry, 1)) {
        return g_realW ? g_realW(key, valueName, reserved, type, data, dataSize)
                       : ERROR_FILE_NOT_FOUND;
    }

    char nameA[256] = {};
    if (valueName)
        WideCharToMultiByte(CP_ACP, 0, valueName, -1, nameA, sizeof(nameA), nullptr, nullptr);
    const bool languageQuery = nameA[0] && _stricmp(nameA, "LNG_Language") == 0;
    if (languageQuery)
        Events().NotifyLanguageRegistryQuery("RegQueryValueExW(LNG_Language)");

    DWORD value = 0;
    LONG result = ERROR_SUCCESS;
    if (LookupValueA(nameA, &value)) {
        if (type) *type = REG_DWORD;
        if (data && dataSize && *dataSize >= sizeof(DWORD)) *(DWORD*)data = value;
        if (dataSize) *dataSize = sizeof(DWORD);

        if (languageQuery) {
            InterlockedExchange(&g_lastLanguageValue, (LONG)value);
            const LONG count = InterlockedIncrement(&g_languageQueryCount);
            if (count == 1)
                Log().Write(2, "LNG_Language intercepted through RegQueryValueExW: returned=%lu",
                            (unsigned long)value);
        }
    } else {
        result = g_realW ? g_realW(key, valueName, reserved, type, data, dataSize)
                         : ERROR_FILE_NOT_FOUND;
    }
    InterlockedExchange(&g_reentry, 0);
    return result;
}

static void RegisterConfig()
{
    Ini().AddSection(L"Language", L"Language");
    Ini().AddInt(L"Language", L"TextLanguage", 6, {
        L"Text/UI/subtitle language returned for LNG_Language.",
        L"0=None/Auto, 1=English, 2=French, 3=Spanish, 4=Polish, 5=German, 6=Chinese,",
        L"7=Hungarian, 8=Italian, 9=Japanese, 10=Czech, 11=Korean, 12=Russian, 13=Dutch."
    });
    Ini().AddInt(L"Language", L"VoiceLanguage", 1, {
        L"Requested dialogue voice language, using the same ID table."
    });


}

static void ValidateConfig()
{
    Ini().ValidateRange("Language", "TextLanguage", 6, 0, 13);
    Ini().ValidateRange("Language", "VoiceLanguage", 1, 1, 13);
}

static bool Initialize() { return true; }

static void RegisterHooks()
{
    if (InterlockedCompareExchange(&g_hooksRegistered, 1, 0) != 0) return;
    Hooks().RegisterIatHook({"ADVAPI32.DLL"}, "RegQueryValueExA",
                            (void*)HookA, (void**)&g_realA, "SettingsRegistry");
    Hooks().RegisterIatHook({"ADVAPI32.DLL"}, "RegQueryValueExW",
                            (void*)HookW, (void**)&g_realW, "SettingsRegistry");
}

void RegisterEarlyHooks() { RegisterHooks(); }
LONG LanguageQueryCount() { return InterlockedCompareExchange(&g_languageQueryCount, 0, 0); }
LONG LastLanguageValue() { return InterlockedCompareExchange(&g_lastLanguageValue, 0, 0); }

static void WriteReport(ReportWriter& report)
{
    report.Line(L"[SettingsRegistry]");
    report.Line(L"TextLanguage: %d", ReadTextLanguage());
    report.Line(L"AspectRatioOverride: %u", (unsigned)ReadAspectRatioOverride());
    report.Line(L"Registry IAT hooks: %d", Hooks().InstalledCountForOwner("SettingsRegistry"));
    report.Line(L"LNG_Language intercept count: %ld", LanguageQueryCount());
    report.Line(L"Last LNG_Language value returned: %ld", LastLanguageValue());
    report.Line(L"");
}

static const Module kModule = {
    L"SettingsRegistry",
    RegisterConfig,
    ValidateConfig,
    Initialize,
    RegisterHooks,
    nullptr,
    nullptr,
    WriteReport,
};

const Module& GetModule() { return kModule; }

} // namespace popopt::settings
