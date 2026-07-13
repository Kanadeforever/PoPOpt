#include "DisplayModule.h"
#include "../core/PeUtils.h"

namespace popopt::display {

static volatile LONG g_windowPatchApplied = 0;

int WindowMode()
{
    return ClampInt(Ini().GetInt("Display", "WindowMode", 2), 0, 2);
}

static void RegisterConfig()
{
    Ini().AddSection(L"Display", L"Display");
    Ini().AddInt(L"Display", L"WindowMode", 2,
                 {L"0=fullscreen, 1=windowed, 2=borderless window at Width x Height (default)."});
    Ini().AddBool(L"Display", L"BorderlessCenter", false,
                  {L"Only mode 2: 0=keep game position, 1=center on the nearest monitor work area."});
    Ini().AddInt(L"Display", L"Width", 1920);
    Ini().AddInt(L"Display", L"Height", 1080);
    Ini().AddBool(L"Display", L"VSync", true);
    Ini().AddBool(L"Display", L"Widescreen", true);
    Ini().AddInt(L"Display", L"AspectRatio", 0,
                 {L"0=auto floor((Width/Height)*100); otherwise set a manual value such as 177."});
}

static void ValidateConfig()
{
    Ini().ValidateRange("Display", "WindowMode", 2, 0, 2);
    Ini().ValidateBool("Display", "BorderlessCenter", 0);
    Ini().ValidateRange("Display", "Width", 1920, 320, 16384);
    Ini().ValidateRange("Display", "Height", 1080, 240, 16384);
    Ini().ValidateBool("Display", "VSync", 1);
    Ini().ValidateBool("Display", "Widescreen", 1);

    int aspect = Ini().GetInt("Display", "AspectRatio", 0);
    int fixed = aspect <= 0 ? 0 : ClampInt(aspect, 50, 500);
    if (aspect != fixed) {
        Ini().SetInt("Display", "AspectRatio", fixed);
        ++App().configCorrections;
        Log().Write(1, "Config corrected: [Display] AspectRatio=%d -> %d", aspect, fixed);
    }
}

static bool Initialize() { return true; }

static void ApplyPatches()
{
    if (WindowMode() == 0) {
        Log().Write(2, "WindowMode=0: fullscreen requested; window patch disabled");
        return;
    }

    static const BYTE expected[20] = {
        '/', 'f','u','l','l','S','c','r','e','e','n',':','o','n',0,0,0,0,0,0
    };
    static const BYTE patch[20] = {
        '/', 'f','u','l','l','S','c','r','e','e','n',':','o','f','f',0,0,0,0,0
    };
    if (pe::PatchByRvaOrScan("WindowMode", 0x00929D9C,
                             expected, sizeof(expected), patch, sizeof(patch))) {
        InterlockedExchange(&g_windowPatchApplied, 1);
        Log().Write(2, "WindowMode=%d active", WindowMode());
    }
}

static BOOL CALLBACK EnumWindowProc(HWND window, LPARAM parameter)
{
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    if (processId != GetCurrentProcessId()) return TRUE;
    if (!IsWindowVisible(window)) return TRUE;
    if (GetWindow(window, GW_OWNER) != nullptr) return TRUE;
    *(HWND*)parameter = window;
    return FALSE;
}

static HWND FindMainWindow()
{
    HWND result = nullptr;
    EnumWindows(EnumWindowProc, (LPARAM)&result);
    return result;
}

static void ApplyBorderless(HWND window)
{
    LONG_PTR style = GetWindowLongPtrW(window, GWL_STYLE);
    LONG_PTR exStyle = GetWindowLongPtrW(window, GWL_EXSTYLE);
    style &= ~(WS_CAPTION | WS_THICKFRAME | WS_BORDER | WS_DLGFRAME);
    exStyle &= ~(WS_EX_DLGMODALFRAME | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE);
    SetWindowLongPtrW(window, GWL_STYLE, style);
    SetWindowLongPtrW(window, GWL_EXSTYLE, exStyle);

    const int width = ClampInt(Ini().GetInt("Display", "Width", 1920), 320, 16384);
    const int height = ClampInt(Ini().GetInt("Display", "Height", 1080), 240, 16384);
    const bool center = Ini().GetInt("Display", "BorderlessCenter", 0) != 0;

    RECT current = {};
    GetWindowRect(window, &current);
    int x = current.left;
    int y = current.top;

    if (center) {
        HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
        MONITORINFO info = {};
        info.cbSize = sizeof(info);
        if (GetMonitorInfoW(monitor, &info)) {
            const int monitorWidth = info.rcWork.right - info.rcWork.left;
            const int monitorHeight = info.rcWork.bottom - info.rcWork.top;
            x = info.rcWork.left + (monitorWidth - width) / 2;
            y = info.rcWork.top + (monitorHeight - height) / 2;
        }
    }

    SetWindowPos(window, HWND_TOP, x, y, width, height,
                 SWP_NOOWNERZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    Log().Write(2, "Borderless window applied: %d,%d %dx%d center=%d",
                x, y, width, height, center ? 1 : 0);
}

static DWORD WINAPI BorderlessThread(LPVOID)
{
    for (int i = 0; i < 600; ++i) {
        if (WindowMode() != 2) return 0;
        HWND window = FindMainWindow();
        if (window) {
            ApplyBorderless(window);
            return 0;
        }
        Sleep(100);
    }
    Log().Write(1, "Borderless: main window not found within timeout");
    return 0;
}

static void Start()
{
    if (WindowMode() != 2) return;
    HANDLE thread = CreateThread(nullptr, 0, BorderlessThread, nullptr, 0, nullptr);
    if (thread) CloseHandle(thread);
    else Log().Write(1, "Borderless thread creation failed, error=%lu", GetLastError());
}

static void WriteReport(ReportWriter& report)
{
    report.Line(L"[Display]");
    report.Line(L"WindowMode: %d", WindowMode());
    report.Line(L"Resolution: %dx%d",
                Ini().GetInt("Display", "Width", 1920),
                Ini().GetInt("Display", "Height", 1080));
    report.Line(L"Window patch applied: %d", (int)g_windowPatchApplied);
    report.Line(L"");
}

static const Module kModule = {
    L"Display", RegisterConfig, ValidateConfig, Initialize,
    nullptr, ApplyPatches, Start, WriteReport
};

const Module& GetModule() { return kModule; }

} // namespace popopt::display
