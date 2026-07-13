#include "VoiceModule.h"
#include "../core/LanguageIds.h"
#include "../core/PeUtils.h"

namespace popopt::voice {

using CreateFileA_t = HANDLE (WINAPI *)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using CreateFileW_t = HANDLE (WINAPI *)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);

static CreateFileA_t g_realCreateFileA = nullptr;
static CreateFileW_t g_realCreateFileW = nullptr;

static int g_effectiveLanguage = -1;
static bool g_runtimeEnabled = false;
static volatile LONG g_fullPatchApplied = 0;
static volatile LONG g_packedRuntimeMode = 0;
static volatile LONG g_packedApplied = 0;
static volatile LONG g_packedThreadStarted = 0;
static volatile LONG g_patchInProgress = 0;
static volatile LONG g_initialized = 0;
static volatile LONG g_hooksRegistered = 0;
static volatile LONG g_patchPhaseStarted = 0;

static const pe::RvaCandidate kPackageAll[] = {
    {"GOG", 0x006C7870}, {"SteamUnpacked", 0x006C7610}
};
static const pe::RvaCandidate kResourceAll[] = {
    {"GOG", 0x008DE8FE}, {"SteamUnpacked", 0x008DE6FE}
};
static const pe::RvaCandidate kBundleAll[] = {
    {"GOG", 0x004F24B4}, {"SteamUnpacked", 0x004F1CA4}
};
static const pe::RvaCandidate kNameAll[] = {
    {"GOG", 0x004F2B5F}, {"SteamUnpacked", 0x004F234F}
};
static const pe::RvaCandidate kPackageSteam[] = {{"SteamRuntime", 0x006C7610}};
static const pe::RvaCandidate kResourceSteam[] = {{"SteamRuntime", 0x008DE6FE}};
static const pe::RvaCandidate kBundleSteam[] = {{"SteamRuntime", 0x004F1CA4}};
static const pe::RvaCandidate kNameSteam[] = {{"SteamRuntime", 0x004F234F}};

static const BYTE kExpectedPackage[] = {
    0x8B,0x44,0x24,0x08,0x85,0xC0,0x75,0x05,0xE8,0xB3,0x74,0xA5,0xFF
};
static const char kMaskPackage[] = "xxxxxxxxx????";

static const BYTE kExpectedResource[] = {
    0xE8,0x2D,0x04,0x84,0xFF,0x50,0xE8,0x67,0x04,0x84,0xFF,
    0x8A,0x16,0x83,0xC4,0x04,0x3A,0x10,0x75,0x14,0x8A,0x4E,0x01,
    0x3A,0x48,0x01,0x75,0x0C,0x8A,0x56,0x02,0x3A,0x50,0x02
};
static const char kMaskResource[] = "x????xx????xxxxxxxxxxxxxxxxxxxxxxx";

static const BYTE kExpectedBundle[] = {
    0xE8,0x77,0xC8,0xC2,0xFF,0x3B,0xF8,0x75,0x70,
    0x8B,0x15,0x24,0x74,0xF3,0x00,0x52,0x6A,0xFF,0x53,
    0xE8,0xA4,0x01,0xC3,0xFF,0x8B,0x0D,0x98,0x55,0xF9,0x00
};
static const char kMaskBundle[] = "x????xxx?xxxxxxxxxxx????xxxxxx";

static const BYTE kExpectedName[] = {
    0xE8,0xCC,0xC1,0xC2,0xFF,0x50,0xE8,0x06,0xC2,0xC2,0xFF,
    0x50,0xE8,0x70,0xF5,0x38,0x00,0x8D,0x4C,0x24,0x18,
    0x68,0xFF,0xFF,0xFF,0x7F,0x51,0xE8,0x31,0xC2,0x38,0x00
};
static const char kMaskName[] = "x????xx????xx????xxxxxxxxxxx????";

int RequestedLanguage()
{
    return ClampInt(Ini().GetInt("Language", "VoiceLanguage", 1), 1, 13);
}

int EffectiveLanguage()
{
    return (g_effectiveLanguage >= 1 && g_effectiveLanguage <= 13)
        ? g_effectiveLanguage : RequestedLanguage();
}

bool RuntimeEnabled() { return g_runtimeEnabled; }

void BuildVoicePackPath(int language, wchar_t* out, int count)
{
    if (!out || count <= 0) return;
    out[0] = 0;
    const wchar_t* suffix = lang::VoiceSuffix(language);
    if (!suffix || !*suffix) return;
#if defined(_MSC_VER)
    _snwprintf_s(out, count, _TRUNCATE, L"%ls\\DataPC_StreamedSounds%ls.forge",
                 App().gameDir, suffix);
#else
    swprintf(out, count, L"%ls\\DataPC_StreamedSounds%ls.forge", App().gameDir, suffix);
#endif
}

bool VoicePackExists(int language)
{
    wchar_t path[kPathCapacity] = {};
    BuildVoicePackPath(language, path, (int)kPathCapacity);
    DWORD attributes = GetFileAttributesW(path);
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

static void ResolveFallback()
{
    const bool enabled = Ini().GetInt("Voice", "Enable", 1) != 0;
    const int requested = RequestedLanguage();
    const bool autoFallback = Ini().GetInt("Voice", "AutoFallback", 1) != 0;
    const int fallback = ClampInt(Ini().GetInt("Voice", "FallbackLanguage", 1), 1, 13);

    g_effectiveLanguage = requested;
    g_runtimeEnabled = enabled;
    if (!enabled) return;

    if (VoicePackExists(requested)) {
        Log().Write(2, "Voice pack detected: requested language=%d", requested);
        return;
    }
    if (autoFallback && fallback != requested && VoicePackExists(fallback)) {
        g_effectiveLanguage = fallback;
        Log().Write(1, "Requested voice pack language=%d is missing; falling back to language=%d",
                    requested, fallback);
        return;
    }

    g_effectiveLanguage = -1;
    g_runtimeEnabled = false;
    if (autoFallback)
        Log().Write(1, "Requested voice pack language=%d and fallback language=%d are unavailable; voice patches disabled",
                    requested, fallback);
    else
        Log().Write(1, "Requested voice pack language=%d is missing and AutoFallback=0; voice patches disabled",
                    requested);
}

static char LowerAscii(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }

static bool ContainsInsensitive(const char* text, const char* needle)
{
    if (!text || !needle || !*needle) return false;
    const size_t length = strlen(needle);
    for (const char* cursor = text; *cursor; ++cursor) {
        size_t i = 0;
        while (i < length && cursor[i] && LowerAscii(cursor[i]) == LowerAscii(needle[i])) ++i;
        if (i == length) return true;
    }
    return false;
}

static bool ShouldTraceFile(const char* path)
{
    return path && (ContainsInsensitive(path, ".forge") ||
                    ContainsInsensitive(path, "Streamed") ||
                    ContainsInsensitive(path, "Sound") ||
                    ContainsInsensitive(path, "DataPC"));
}

static void BuildPatchBytes(int language, BYTE package[13], BYTE resource[5],
                            BYTE bundle[5], BYTE name[5])
{
    const BYTE packageTemplate[13] = {
        0xB8,0x01,0x00,0x00,0x00,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90
    };
    const BYTE movTemplate[5] = {0xB8,0x01,0x00,0x00,0x00};
    memcpy(package, packageTemplate, 13);
    memcpy(resource, movTemplate, 5);
    memcpy(bundle, movTemplate, 5);
    memcpy(name, movTemplate, 5);
    *(DWORD*)(package + 1) = (DWORD)language;
    *(DWORD*)(resource + 1) = (DWORD)language;
    *(DWORD*)(bundle + 1) = (DWORD)language;
    *(DWORD*)(name + 1) = (DWORD)language;
}

static BYTE* FindCandidateSilent(const pe::RvaCandidate* candidates, int count,
                                 const BYTE* expected, const char* mask, DWORD size)
{
    BYTE* base = nullptr;
    DWORD imageSize = 0;
    if (!pe::GetImageRange(GetModuleHandleW(nullptr), &base, &imageSize)) return nullptr;
    for (int i = 0; i < count; ++i) {
        const DWORD rva = candidates[i].rva;
        if (rva + size <= imageSize && pe::MatchMaskAt(base + rva, expected, mask, size))
            return base + rva;
    }
    return nullptr;
}

static bool CandidateSetReady(const pe::RvaCandidate* package, int packageCount,
                              const pe::RvaCandidate* resource, int resourceCount,
                              const pe::RvaCandidate* bundle, int bundleCount,
                              const pe::RvaCandidate* name, int nameCount)
{
    return FindCandidateSilent(package, packageCount, kExpectedPackage, kMaskPackage, sizeof(kExpectedPackage)) &&
           FindCandidateSilent(resource, resourceCount, kExpectedResource, kMaskResource, sizeof(kExpectedResource)) &&
           FindCandidateSilent(bundle, bundleCount, kExpectedBundle, kMaskBundle, sizeof(kExpectedBundle)) &&
           FindCandidateSilent(name, nameCount, kExpectedName, kMaskName, sizeof(kExpectedName));
}

static bool ApplyFullPatch(const char* group,
                           const pe::RvaCandidate* package, int packageCount,
                           const pe::RvaCandidate* resource, int resourceCount,
                           const pe::RvaCandidate* bundle, int bundleCount,
                           const pe::RvaCandidate* name, int nameCount,
                           int language)
{
    BYTE patchPackage[13], patchResource[5], patchBundle[5], patchName[5];
    BuildPatchBytes(language, patchPackage, patchResource, patchBundle, patchName);

    pe::PatchItem items[4] = {};
    items[0] = {"VoicePackageLanguage", 0, package, packageCount,
                kExpectedPackage, kMaskPackage, sizeof(kExpectedPackage),
                patchPackage, sizeof(patchPackage), false};
    items[1] = {"VoiceResourceLanguage", 0, resource, resourceCount,
                kExpectedResource, kMaskResource, sizeof(kExpectedResource),
                patchResource, sizeof(patchResource), false};
    items[2] = {"VoiceBundleLanguage", 0, bundle, bundleCount,
                kExpectedBundle, kMaskBundle, sizeof(kExpectedBundle),
                patchBundle, sizeof(patchBundle), false};
    items[3] = {"VoiceNameLanguage", 0, name, nameCount,
                kExpectedName, kMaskName, sizeof(kExpectedName),
                patchName, sizeof(patchName), false};
    return pe::ApplyPatchGroupAtomic(group, items, 4);
}

static bool TryApplyPacked(const char* reason, bool logMiss)
{
    if (!g_packedRuntimeMode) return false;
    if (g_packedApplied) return true;
    if (InterlockedCompareExchange(&g_patchInProgress, 1, 0) != 0) return false;

    bool result = false;
    const bool assetPatch = Ini().GetInt("Voice", "AssetPatch", 1) != 0;
    const bool requireAssetPatch = Ini().GetInt("Voice", "RequireAssetPatch", 1) != 0;
    if (!g_runtimeEnabled || !assetPatch || !requireAssetPatch) {
        if (logMiss) Log().Write(2, "Packed Steam voice patch skipped by configuration");
    } else if (!CandidateSetReady(kPackageSteam, 1, kResourceSteam, 1,
                                  kBundleSteam, 1, kNameSteam, 1)) {
        if (logMiss) Log().Write(2, "Packed Steam voice runtime targets are not ready: reason=%s",
                                 reason ? reason : "unknown");
    } else {
        result = ApplyFullPatch("VoiceFullPatch",
                                kPackageSteam, 1, kResourceSteam, 1,
                                kBundleSteam, 1, kNameSteam, 1,
                                EffectiveLanguage());
        if (result) {
            InterlockedExchange(&g_packedApplied, 1);
            InterlockedExchange(&g_fullPatchApplied, 1);
            Log().Write(2, "Packed Steam voice runtime patch applied: language=%d reason=%s",
                        EffectiveLanguage(), reason ? reason : "unknown");
            Events().RequestReportRefresh();
        }
    }

    InterlockedExchange(&g_patchInProgress, 0);
    return result;
}

static void TryPackedFast(const char* reason)
{
    if (!g_packedRuntimeMode || g_packedApplied || g_patchInProgress) return;
    TryApplyPacked(reason, false);
}

static void OnLanguageRegistryQuery(const char* reason) { TryPackedFast(reason); }

static DWORD WINAPI PackedThread(LPVOID)
{
    int tries = ClampInt(Ini().GetInt("Debug", "PackedSteamPatchTries", 10000), 1, 60000);
    int interval = ClampInt(Ini().GetInt("Debug", "PackedSteamPatchIntervalMs", 1), 1, 1000);
    int statusEvery = ClampInt(Ini().GetInt("Debug", "PackedSteamPatchStatusEvery", 1000), 1, 60000);
    Log().Write(2, "Packed Steam runtime patch thread started: tries=%d interval=%dms", tries, interval);
    for (int i = 1; i <= tries; ++i) {
        if (TryApplyPacked("runtime-thread", i == 1 || i % statusEvery == 0)) {
            Log().Write(2, "Packed Steam runtime patch thread complete after %d pass(es)", i);
            return 0;
        }
        Sleep((DWORD)interval);
    }
    Log().Write(1, "Packed Steam runtime patch thread ended without applying voice patch");
    return 0;
}

static void StartPackedThread()
{
    if (!g_packedRuntimeMode || g_packedApplied) return;
    if (InterlockedCompareExchange(&g_packedThreadStarted, 1, 0) != 0) return;
    HANDLE thread = CreateThread(nullptr, 0, PackedThread, nullptr, 0, nullptr);
    if (thread) CloseHandle(thread);
    else {
        InterlockedExchange(&g_packedThreadStarted, 0);
        Log().Write(1, "Packed Steam runtime thread creation failed, error=%lu", GetLastError());
    }
}

static HANDLE WINAPI HookCreateFileA(LPCSTR path, DWORD access, DWORD share,
                                     LPSECURITY_ATTRIBUTES security, DWORD creation,
                                     DWORD flags, HANDLE templateFile)
{
    if (ShouldTraceFile(path)) TryPackedFast("CreateFileA");
    HANDLE result = g_realCreateFileA
        ? g_realCreateFileA(path, access, share, security, creation, flags, templateFile)
        : INVALID_HANDLE_VALUE;
    if (Log().Level() >= 3 && ShouldTraceFile(path))
        Log().Write(3, "CreateFileA %s -> %s", path ? path : "(null)",
                    result == INVALID_HANDLE_VALUE ? "FAIL" : "OK");
    return result;
}

static HANDLE WINAPI HookCreateFileW(LPCWSTR path, DWORD access, DWORD share,
                                     LPSECURITY_ATTRIBUTES security, DWORD creation,
                                     DWORD flags, HANDLE templateFile)
{
    char utf8[kPathCapacity] = {};
    if (path) WideToUtf8(path, utf8, (int)sizeof(utf8));
    if (ShouldTraceFile(utf8)) TryPackedFast("CreateFileW");
    HANDLE result = g_realCreateFileW
        ? g_realCreateFileW(path, access, share, security, creation, flags, templateFile)
        : INVALID_HANDLE_VALUE;
    if (Log().Level() >= 3 && ShouldTraceFile(utf8))
        Log().Write(3, "CreateFileW %s -> %s", utf8,
                    result == INVALID_HANDLE_VALUE ? "FAIL" : "OK");
    return result;
}

static void RegisterConfig()
{
    Ini().AddSection(L"Voice", L"Voice");
    Ini().AddBool(L"Voice", L"Enable", true,
                  {L"Enables independent text and voice languages."});
    Ini().AddBool(L"Voice", L"AssetPatch", true,
                  {L"Keep enabled: patches Package, Resource, Bundle and Name selectors."});
    Ini().AddBool(L"Voice", L"RequireAssetPatch", true,
                  {L"Keep enabled for transactional all-or-nothing voice patching."});
    Ini().AddBool(L"Voice", L"AutoFallback", true,
                  {L"Use FallbackLanguage when the requested voice forge is missing."});
    Ini().AddInt(L"Voice", L"FallbackLanguage", 1);
}

static void ValidateConfig()
{
    Ini().ValidateBool("Voice", "Enable", 1);
    Ini().ValidateBool("Voice", "AssetPatch", 1);
    Ini().ValidateBool("Voice", "RequireAssetPatch", 1);
    Ini().ValidateBool("Voice", "AutoFallback", 1);
    Ini().ValidateRange("Voice", "FallbackLanguage", 1, 1, 13);
    Ini().ValidateRange("Debug", "PackedSteamPatchTries", 10000, 1, 60000);
    Ini().ValidateRange("Debug", "PackedSteamPatchIntervalMs", 1, 1, 1000);
    Ini().ValidateRange("Debug", "PackedSteamPatchStatusEvery", 1000, 1, 60000);
}

static bool Initialize()
{
    if (InterlockedCompareExchange(&g_initialized, 1, 0) != 0) return true;
    ResolveFallback();
    Events().SubscribeLanguageRegistryQuery(OnLanguageRegistryQuery);
    return true;
}

static void RegisterHooks()
{
    if (InterlockedCompareExchange(&g_hooksRegistered, 1, 0) != 0) return;
    Hooks().RegisterIatHook({"KERNEL32.DLL", "KERNELBASE.DLL"}, "CreateFileA",
                            (void*)HookCreateFileA, (void**)&g_realCreateFileA, "Voice");
    Hooks().RegisterIatHook({"KERNEL32.DLL", "KERNELBASE.DLL"}, "CreateFileW",
                            (void*)HookCreateFileW, (void**)&g_realCreateFileW, "Voice");
}

static void ApplyPatches()
{
    if (InterlockedCompareExchange(&g_patchPhaseStarted, 1, 0) != 0) return;
    Log().Write(2, "VoiceLanguage=%d", EffectiveLanguage());
    if (!g_runtimeEnabled) {
        Log().Write(1, "Voice patch disabled by config or missing voice packages");
        return;
    }

    if (App().exeFlavor == ExecutableFlavor::SteamPackedStub) {
        InterlockedExchange(&g_packedRuntimeMode, 1);
        Log().Write(2, "Packed SteamStub detected: voice patch waits for runtime unpacking");
        TryApplyPacked("startup", true);
        return;
    }

    const bool assetPatch = Ini().GetInt("Voice", "AssetPatch", 1) != 0;
    const bool requireAssetPatch = Ini().GetInt("Voice", "RequireAssetPatch", 1) != 0;
    if (assetPatch && requireAssetPatch) {
        Log().Write(2, "Voice FullPatch=1: applying four selectors transactionally");
        if (ApplyFullPatch("VoiceFullPatch",
                           kPackageAll, 2, kResourceAll, 2,
                           kBundleAll, 2, kNameAll, 2,
                           EffectiveLanguage())) {
            InterlockedExchange(&g_fullPatchApplied, 1);
            Log().Write(2, "Voice language=%d active", EffectiveLanguage());
        } else {
            Log().Write(1, "VoiceFullPatch failed; no partial voice patch was applied");
        }
        return;
    }

    Log().Write(1, "Legacy partial voice configuration selected; transactional safety is reduced");
    BYTE package[13], resource[5], bundle[5], name[5];
    BuildPatchBytes(EffectiveLanguage(), package, resource, bundle, name);
    pe::PatchByCandidatesMasked("VoicePackageLanguage", kPackageAll, 2,
                                kExpectedPackage, kMaskPackage, sizeof(kExpectedPackage),
                                package, sizeof(package), false);
    if (assetPatch) {
        pe::PatchItem items[3] = {};
        items[0] = {"VoiceResourceLanguage", 0, kResourceAll, 2,
                    kExpectedResource, kMaskResource, sizeof(kExpectedResource),
                    resource, sizeof(resource), false};
        items[1] = {"VoiceBundleLanguage", 0, kBundleAll, 2,
                    kExpectedBundle, kMaskBundle, sizeof(kExpectedBundle),
                    bundle, sizeof(bundle), false};
        items[2] = {"VoiceNameLanguage", 0, kNameAll, 2,
                    kExpectedName, kMaskName, sizeof(kExpectedName),
                    name, sizeof(name), false};
        pe::ApplyPatchGroupAtomic("VoiceAssetPatch", items, 3);
    }
}

bool InitializeEarly() { return Initialize(); }
void RegisterEarlyHooks() { RegisterHooks(); }
void ApplyEarlyPatches() { ApplyPatches(); }

static void Start() { StartPackedThread(); }

static void WriteReport(ReportWriter& report)
{
    report.Line(L"[Voice]");
    report.Line(L"Requested language: %d (%s)", RequestedLanguage(), lang::Name(RequestedLanguage()));
    report.Line(L"Effective language: %d (%s)", g_effectiveLanguage, lang::Name(g_effectiveLanguage));
    report.Line(L"Runtime enabled: %d", g_runtimeEnabled ? 1 : 0);
    report.Line(L"Full patch applied: %d", (int)g_fullPatchApplied);
    report.Line(L"Packed runtime mode: %d", (int)g_packedRuntimeMode);
    report.Line(L"Packed patch applied: %d", (int)g_packedApplied);
    report.Line(L"Voice file hooks: %d", Hooks().InstalledCountForOwner("Voice"));
    report.Line(L"Detected voice packages:");
    for (int id = 1; id <= 13; ++id)
        report.Line(L"  %2d %-12s : %s", id, lang::Name(id), VoicePackExists(id) ? L"FOUND" : L"missing");
    report.Line(L"");
}

static const Module kModule = {
    L"Voice",
    RegisterConfig,
    ValidateConfig,
    Initialize,
    RegisterHooks,
    ApplyPatches,
    Start,
    WriteReport,
};

const Module& GetModule() { return kModule; }

} // namespace popopt::voice
