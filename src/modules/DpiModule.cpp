#include "DpiModule.h"

namespace popopt::dpi {

static std::wstring g_activeMode = L"disabled";

static void RegisterConfig()
{
    Ini().AddSection(L"DPI", L"DPI");
    Ini().AddBool(L"DPI", L"DPIAware", true);
    Ini().AddInt(L"DPI", L"DPIAwareness", 1,
                 {L"1=System DPI (recommended), 2=Per-monitor, 3=Per-monitor V2."});
}

static void ValidateConfig()
{
    Ini().ValidateBool("DPI", "DPIAware", 1);
    Ini().ValidateRange("DPI", "DPIAwareness", 1, 1, 3);
}

static bool Initialize()
{
    if (!Ini().GetInt("DPI", "DPIAware", 1)) {
        g_activeMode = L"disabled";
        Log().Write(2, "DPI awareness disabled");
        return true;
    }

    const int mode = ClampInt(Ini().GetInt("DPI", "DPIAwareness", 1), 1, 3);
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) user32 = LoadLibraryW(L"user32.dll");

    if (mode >= 3 && user32) {
        using SetProcessDpiAwarenessContext_t = BOOL (WINAPI *)(HANDLE);
        auto function = (SetProcessDpiAwarenessContext_t)GetProcAddress(
            user32, "SetProcessDpiAwarenessContext");
        if (function && function((HANDLE)(LONG_PTR)-4)) {
            g_activeMode = L"Per-Monitor V2";
            Log().Write(2, "DPI awareness active: Per-Monitor V2");
            return true;
        }
    }

    if (mode >= 2) {
        HMODULE shcore = GetModuleHandleW(L"shcore.dll");
        if (!shcore) shcore = LoadLibraryW(L"shcore.dll");
        if (shcore) {
            using SetProcessDpiAwareness_t = HRESULT (WINAPI *)(int);
            auto function = (SetProcessDpiAwareness_t)GetProcAddress(
                shcore, "SetProcessDpiAwareness");
            if (function && SUCCEEDED(function(2))) {
                g_activeMode = L"Per-Monitor";
                Log().Write(2, "DPI awareness active: Per-Monitor");
                return true;
            }
        }
    }

    if (user32) {
        using SetProcessDPIAware_t = BOOL (WINAPI *)(void);
        auto function = (SetProcessDPIAware_t)GetProcAddress(user32, "SetProcessDPIAware");
        if (function && function()) {
            g_activeMode = L"System";
            Log().Write(2, "DPI awareness active: System");
            return true;
        }
    }

    g_activeMode = L"failed";
    Log().Write(1, "DPI awareness could not be set");
    return true;
}

static void WriteReport(ReportWriter& report)
{
    report.Line(L"[DPI]");
    report.Line(L"Active mode: %s", g_activeMode.c_str());
    report.Line(L"");
}

static const Module kModule = {
    L"DPI", RegisterConfig, ValidateConfig, Initialize,
    nullptr, nullptr, nullptr, WriteReport
};
const Module& GetModule() { return kModule; }

} // namespace popopt::dpi
