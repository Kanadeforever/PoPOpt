#include "DiagnosticsModule.h"
#include "../core/PeUtils.h"

namespace popopt::diagnostics {

static std::vector<const Module*> g_modules;
static volatile LONG g_reportThreadStarted = 0;
static SRWLOCK g_reportLock = SRWLOCK_INIT;

void SetModuleList(const Module* const* modules, size_t count)
{
    g_modules.assign(modules, modules + count);
}

static void RegisterConfig()
{
    Ini().AddSection(L"Debug", L"Debug / diagnostics");
    Ini().AddInt(L"Debug", L"LogLevel", 2, {
        L"0=off and delete old log; 1=warnings/errors; 2=normal; 3=verbose tracing."
    });
    Ini().AddBool(L"Debug", L"CompatibilityReport", true,
                  {L"Writes PoP_CompatibilityReport.txt."});
    Ini().AddInt(L"Debug", L"PackedSteamPatchTries", 10000,
                 {L"Packed-Steam runtime voice patch controls; normal users should keep defaults."});
    Ini().AddInt(L"Debug", L"PackedSteamPatchIntervalMs", 1);
    Ini().AddInt(L"Debug", L"PackedSteamPatchStatusEvery", 1000);
    Ini().AddBool(L"Debug", L"TextureLogUnmatched", false,
                  {L"Verbose texture hash diagnostics; does not change matching behavior."});
    Ini().AddInt(L"Debug", L"TextureMaxUnmatchedLogs", 256);
}

static void ValidateConfig()
{
    int raw = Ini().GetInt("Debug", "LogLevel", 2);
    int fixed = ClampInt(raw, 0, 3);
    if (raw != fixed) {
        Ini().SetInt("Debug", "LogLevel", fixed);
        ++App().configCorrections;
    }
    App().logLevel = fixed;
    Log().SetLevel(fixed);

    Ini().ValidateBool("Debug", "CompatibilityReport", 1);
    Ini().ValidateRange("Debug", "PackedSteamPatchTries", 10000, 1, 60000);
    Ini().ValidateRange("Debug", "PackedSteamPatchIntervalMs", 1, 1, 1000);
    Ini().ValidateRange("Debug", "PackedSteamPatchStatusEvery", 1000, 1, 60000);
    Ini().ValidateBool("Debug", "TextureLogUnmatched", 0);
    Ini().ValidateRange("Debug", "TextureMaxUnmatchedLogs", 256, 0, 100000);
}

static bool Initialize()
{
    Events().SubscribeReportRefresh(WriteNow);
    return true;
}

void WriteNow()
{
    if (!Ini().GetInt("Debug", "CompatibilityReport", 1)) return;
    AcquireSRWLockExclusive(&g_reportLock);

    HANDLE file = CreateFileW(App().reportPath, GENERIC_WRITE, FILE_SHARE_READ,
                              nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        ReleaseSRWLockExclusive(&g_reportLock);
        Log().Write(1, "Compatibility report could not be created, error=%lu", GetLastError());
        return;
    }

    DWORD written = 0;
    const WORD bom = 0xFEFF;
    WriteFile(file, &bom, sizeof(bom), &written, nullptr);
    ReportWriter report(file);

    wchar_t exePath[kPathCapacity] = {};
    GetModuleFileNameW(nullptr, exePath, (DWORD)kPathCapacity);
    WIN32_FILE_ATTRIBUTE_DATA attributes = {};
    GetFileAttributesExW(exePath, GetFileExInfoStandard, &attributes);
    ULONGLONG exeSize = ((ULONGLONG)attributes.nFileSizeHigh << 32) | attributes.nFileSizeLow;

    report.Line(L"PoPOpt / PoP Universal Patch v29 modular experimental report");
    report.Line(L"================================================================");
    report.Line(L"Game EXE: %s", exePath);
    report.Line(L"EXE flavor: %s", pe::FlavorName(App().exeFlavor));
    report.Line(L"Entry RVA: 0x%08X", pe::GetEntryPointRva());
    report.Line(L"EXE size: %llu bytes", (unsigned long long)exeSize);
    report.Line(L"ASI directory: %s", App().asiDir);
    report.Line(L"Config path: %s", App().iniPath);
    report.Line(L"Log path: %s", App().logPath);
    report.Line(L"");

    report.Line(L"[Core]");
    report.Line(L"Configuration corrections this run: %d", App().configCorrections);
    report.Line(L"LogLevel: %d", App().logLevel);
    report.Line(L"");

    for (const Module* module : g_modules) {
        if (module && module->WriteReport) module->WriteReport(report);
    }

    CloseHandle(file);
    ReleaseSRWLockExclusive(&g_reportLock);
    Log().Write(2, "Compatibility report written");
}

static DWORD WINAPI ReportThread(LPVOID)
{
    Sleep(2000);
    WriteNow();
    return 0;
}

static void Start()
{
    if (!Ini().GetInt("Debug", "CompatibilityReport", 1)) return;
    if (InterlockedCompareExchange(&g_reportThreadStarted, 1, 0) != 0) return;
    HANDLE thread = CreateThread(nullptr, 0, ReportThread, nullptr, 0, nullptr);
    if (thread) CloseHandle(thread);
    else InterlockedExchange(&g_reportThreadStarted, 0);
}

static void WriteReport(ReportWriter&) {}

static const Module kModule = {
    L"Diagnostics", RegisterConfig, ValidateConfig, Initialize,
    nullptr, nullptr, Start, WriteReport
};

const Module& GetModule() { return kModule; }

} // namespace popopt::diagnostics
