#include "core/Core.h"
#include "core/Module.h"
#include "core/PeUtils.h"

#include "modules/SettingsRegistryModule.h"
#include "modules/VoiceModule.h"
#include "modules/DisplayModule.h"
#include "modules/DpiModule.h"
#include "modules/CpuModule.h"
#include "modules/InputModule.h"
#include "modules/GraphicsModule.h"
#include "modules/TextureLoaderModule.h"
#include "modules/DiagnosticsModule.h"

namespace popopt {

static const Module* g_modules[] = {
    &settings::GetModule(),
    &voice::GetModule(),
    &display::GetModule(),
    &dpi::GetModule(),
    &cpu::GetModule(),
    &input::GetModule(),
    &graphics::GetModule(),
    &textureloader::GetModule(),
    &diagnostics::GetModule(),
};

static volatile LONG g_earlyFoundationState = 0; // 0=not started, 1=running, 2=ready

static void RegisterAllConfig()
{
    Ini().AddHeaderComment(L"============================================================");
    Ini().AddHeaderComment(L"PoPOpt / PoP Universal Patch v29 modular experimental");
    Ini().AddHeaderComment(L"Runtime-only ASI patch; the game EXE is never edited on disk.");
    Ini().AddHeaderComment(L"GOG, Steam unpacked and SteamStub packed EXEs are auto-detected.");
    Ini().AddHeaderComment(L"TextureLoader is an isolated optional module and is disabled by default.");
    Ini().AddHeaderComment(L"============================================================");
    for (const Module* module : g_modules) {
        if (module && module->RegisterConfig) module->RegisterConfig();
    }
}

// Language and voice are startup-critical. The game may query LNG_Language and
// cache it immediately after the ASI loader returns from LoadLibrary. Therefore
// these phases must complete synchronously before DllMain returns:
//   - fixed paths and INI access
//   - logging (so early operations remain diagnosable)
//   - EXE flavor detection
//   - voice-package resolution / fallback
//   - LNG_Language + voice file IAT hooks
//   - static voice selector patching, or packed-Steam runtime preparation
// No texture, D3D9, DirectInput, DPI, window or report work is performed here.
static bool InitializeEarlyLanguageVoiceFoundation(HMODULE asiModule)
{
    if (InterlockedCompareExchange(&g_earlyFoundationState, 1, 0) != 0)
        return InterlockedCompareExchange(&g_earlyFoundationState, 0, 0) == 2;

    InitializeFixedPaths(asiModule);

    const int initialLogLevel = ClampInt(Ini().GetInt("Debug", "LogLevel", 2), 0, 3);
    App().logLevel = initialLogLevel;
    Log().Initialize(App().logPath, initialLogLevel);
    Log().ClearOldFile();

    if (initialLogLevel > 0) {
        Log().Write(2, "loaded (v29 modular experimental; early language/voice foundation)");
        char iniUtf8[kPathCapacity] = {};
        WideToUtf8(App().iniPath, iniUtf8, sizeof(iniUtf8));
        Log().Write(2, "ini=%s", iniUtf8);
    }

    pe::DetectExecutableFlavor(true);

    if (!voice::InitializeEarly()) {
        Log().Write(1, "Early voice initialization failed");
        InterlockedExchange(&g_earlyFoundationState, 2);
        return false;
    }

    // Register only the startup-critical hooks before the game can query them.
    settings::RegisterEarlyHooks();
    voice::RegisterEarlyHooks();
    Hooks().InstallMainModuleHooks();

    Log().Write(2, "TextLanguage=%d prepared before DllMain return",
                settings::ReadTextLanguage());

    // GOG/Steam-unpacked selectors are patched now. Packed SteamStub is placed
    // into runtime mode now and completes through the early registry/file hooks
    // or its worker thread after the stub restores the game code.
    voice::ApplyEarlyPatches();

    InterlockedExchange(&g_earlyFoundationState, 2);
    return true;
}

static DWORD WINAPI InitializeThread(LPVOID parameter)
{
    HMODULE asiModule = (HMODULE)parameter;
    if (InterlockedCompareExchange(&g_earlyFoundationState, 0, 0) != 2)
        InitializeEarlyLanguageVoiceFoundation(asiModule);

    RegisterAllConfig();
    Ini().EnsureDefaultFile();

    for (const Module* module : g_modules) {
        if (module && module->ValidateConfig) module->ValidateConfig();
    }
    App().logLevel = Log().Level();

    diagnostics::SetModuleList(g_modules, sizeof(g_modules) / sizeof(g_modules[0]));

    // SettingsRegistry and Voice are idempotent and therefore safely no-op here
    // after their synchronous DllMain phases. All other modules initialize now.
    for (const Module* module : g_modules) {
        if (module && module->Initialize && !module->Initialize()) {
            char name[256] = {};
            WideToUtf8(module->name, name, sizeof(name));
            Log().Write(1, "Module initialization failed: %s", name);
        }
    }

    for (const Module* module : g_modules) {
        if (module && module->RegisterHooks) module->RegisterHooks();
    }
    Hooks().InstallMainModuleHooks();

    Log().Write(2,
                "TextLanguage=%d registry hook ready; LNG_Language queries observed so far=%ld lastValue=%ld",
                settings::ReadTextLanguage(), settings::LanguageQueryCount(),
                settings::LastLanguageValue());

    for (const Module* module : g_modules) {
        if (module && module->ApplyPatches) module->ApplyPatches();
    }
    for (const Module* module : g_modules) {
        if (module && module->Start) module->Start();
    }

    return 0;
}

} // namespace popopt

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);

        // Deliberately synchronous: text and voice are foundational and the
        // game can cache its language before a background thread gets scheduled.
        popopt::InitializeEarlyLanguageVoiceFoundation(instance);

        HANDLE thread = CreateThread(nullptr, 0, popopt::InitializeThread,
                                     instance, 0, nullptr);
        if (thread) CloseHandle(thread);
    }
    return TRUE;
}

// MinGW-w64 static CRT workaround
extern "C" int __mingw_SEH_error_handler(void* ex){return 0;}
