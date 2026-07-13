#include <unknwn.h>
#include "InputModule.h"

namespace popopt::input {

using DirectInput8Create_t = HRESULT (WINAPI *)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
using DI8_CreateDevice_t = HRESULT (STDMETHODCALLTYPE *)(void*, REFGUID, void**, IUnknown*);
using DIDevice_SetCooperativeLevel_t = HRESULT (STDMETHODCALLTYPE *)(void*, HWND, DWORD);

static DirectInput8Create_t g_realDirectInput8Create = nullptr;
static DI8_CreateDevice_t g_realCreateDevice = nullptr;
static DIDevice_SetCooperativeLevel_t g_realSetCooperativeLevel = nullptr;
static volatile LONG g_createDeviceHooked = 0;
static volatile LONG g_setCoopHooked = 0;
static volatile LONG g_focusThreadStarted = 0;

static constexpr DWORD kDisclNoWinKey = 0x00000010;
static const GUID kIidDirectInput8A =
    {0xBF798030,0x483A,0x4DA2,{0xAA,0x99,0x5D,0x64,0xED,0x36,0x97,0x00}};

static bool WindowedMode()
{
    return ClampInt(Ini().GetInt("Display", "WindowMode", 2), 0, 2) != 0;
}

static bool AllowWinKey()
{
    return WindowedMode() && Ini().GetInt("Input", "AllowWinKeyInWindowed", 1) != 0;
}

static bool ReleaseMouseOnFocusLoss()
{
    return WindowedMode() && Ini().GetInt("Input", "ReleaseMouseOnFocusLoss", 1) != 0;
}

static bool ModuleNameContains(HMODULE module, const wchar_t* needle)
{
    wchar_t path[kPathCapacity] = {};
    if (!module || !needle || !GetModuleFileNameW(module, path, (DWORD)kPathCapacity)) return false;
    std::wstring lowerPath = path;
    std::wstring lowerNeedle = needle;
    std::transform(lowerPath.begin(), lowerPath.end(), lowerPath.begin(),
                   [](wchar_t c) { return (wchar_t)towlower(c); });
    std::transform(lowerNeedle.begin(), lowerNeedle.end(), lowerNeedle.begin(),
                   [](wchar_t c) { return (wchar_t)towlower(c); });
    return lowerPath.find(lowerNeedle) != std::wstring::npos;
}

static HRESULT STDMETHODCALLTYPE HookSetCooperativeLevel(void* self, HWND window, DWORD flags)
{
    if (AllowWinKey() && (flags & kDisclNoWinKey)) {
        flags &= ~kDisclNoWinKey;
        static volatile LONG logged = 0;
        if (InterlockedCompareExchange(&logged, 1, 0) == 0)
            Log().Write(2, "DirectInput DISCL_NOWINKEY cleared in windowed/borderless mode");
    }
    return g_realSetCooperativeLevel
        ? g_realSetCooperativeLevel(self, window, flags) : E_FAIL;
}

static HRESULT STDMETHODCALLTYPE HookCreateDevice(void* self, REFGUID guid,
                                                  void** output, IUnknown* outer)
{
    HRESULT result = g_realCreateDevice
        ? g_realCreateDevice(self, guid, output, outer) : E_FAIL;
    if (SUCCEEDED(result) && output && *output && AllowWinKey()) {
        void** vtable = *(void***)(*output);
        if (HookManager::PatchVtableEntry(vtable, 13,
                                          (void*)HookSetCooperativeLevel,
                                          (void**)&g_realSetCooperativeLevel,
                                          "IDirectInputDevice8::SetCooperativeLevel")) {
            if (InterlockedCompareExchange(&g_setCoopHooked, 1, 0) == 0)
                Log().Write(2, "DirectInput device SetCooperativeLevel vtable hooked");
        }
    }
    return result;
}

static void PatchDirectInputObject(void* object)
{
    if (!object || !AllowWinKey()) return;
    void** vtable = *(void***)object;
    if (HookManager::PatchVtableEntry(vtable, 3, (void*)HookCreateDevice,
                                      (void**)&g_realCreateDevice,
                                      "IDirectInput8::CreateDevice")) {
        if (InterlockedCompareExchange(&g_createDeviceHooked, 1, 0) == 0)
            Log().Write(2, "DirectInput8 CreateDevice vtable hooked");
    }
}

static HRESULT WINAPI HookDirectInput8Create(HINSTANCE instance, DWORD version,
                                             REFIID iid, LPVOID* output, LPUNKNOWN outer)
{
    if (!g_realDirectInput8Create) return E_FAIL;
    HRESULT result = g_realDirectInput8Create(instance, version, iid, output, outer);
    if (SUCCEEDED(result) && output && *output && AllowWinKey()) {
        if (memcmp(&iid, &kIidDirectInput8A, sizeof(GUID)) != 0) {
            static volatile LONG logged = 0;
            if (InterlockedCompareExchange(&logged, 1, 0) == 0)
                Log().Write(2, "DirectInput8Create returned non-A IID; attempting compatible vtable hook");
        }
        PatchDirectInputObject(*output);
        static volatile LONG logged = 0;
        if (InterlockedCompareExchange(&logged, 1, 0) == 0)
            Log().Write(2, "DirectInput8Create intercepted without hard dinput dependency");
    }
    return result;
}

static FARPROC DynamicResolver(HMODULE module, LPCSTR name, FARPROC original)
{
    if (!name || !original || (ULONG_PTR)name <= 0xFFFF) return original;
    if (_stricmp(name, "DirectInput8Create") == 0 && ModuleNameContains(module, L"dinput8")) {
        g_realDirectInput8Create = (DirectInput8Create_t)original;
        static volatile LONG logged = 0;
        if (InterlockedCompareExchange(&logged, 1, 0) == 0)
            Log().Write(2, "GetProcAddress hook: DirectInput8Create wrapped");
        return (FARPROC)HookDirectInput8Create;
    }
    return original;
}

static DWORD WINAPI FocusThread(LPVOID)
{
    bool wasUnfocused = false;
    for (;;) {
        if (!ReleaseMouseOnFocusLoss()) return 0;
        HWND foreground = GetForegroundWindow();
        DWORD foregroundPid = 0;
        if (foreground) GetWindowThreadProcessId(foreground, &foregroundPid);
        const bool unfocused = foregroundPid != GetCurrentProcessId();
        if (unfocused) {
            ClipCursor(nullptr);
            if (!wasUnfocused) Log().Write(3, "Mouse released after game focus loss");
        }
        wasUnfocused = unfocused;
        Sleep(50);
    }
}

static void RegisterConfig()
{
    Ini().AddSection(L"Input", L"Input");
    Ini().AddBool(L"Input", L"AllowWinKeyInWindowed", true,
                  {L"Windowed/borderless only: clears DirectInput DISCL_NOWINKEY."});
    Ini().AddBool(L"Input", L"ReleaseMouseOnFocusLoss", true,
                  {L"Windowed/borderless only: releases ClipCursor while the game is unfocused."});
}

static void ValidateConfig()
{
    Ini().ValidateBool("Input", "AllowWinKeyInWindowed", 1);
    Ini().ValidateBool("Input", "ReleaseMouseOnFocusLoss", 1);
}

static bool Initialize() { return true; }

static void RegisterHooks()
{
    if (AllowWinKey()) {
        Hooks().RegisterIatHook({"DINPUT8.DLL"}, "DirectInput8Create",
                                (void*)HookDirectInput8Create,
                                (void**)&g_realDirectInput8Create, "Input");
        Hooks().RegisterDynamicResolver(DynamicResolver);
    }
}

static void Start()
{
    if (!ReleaseMouseOnFocusLoss()) return;
    if (InterlockedCompareExchange(&g_focusThreadStarted, 1, 0) != 0) return;
    HANDLE thread = CreateThread(nullptr, 0, FocusThread, nullptr, 0, nullptr);
    if (thread) {
        CloseHandle(thread);
        Log().Write(2, "Focus-loss mouse release enabled for windowed/borderless mode");
    } else {
        InterlockedExchange(&g_focusThreadStarted, 0);
        Log().Write(1, "Focus-loss mouse release thread failed, error=%lu", GetLastError());
    }
}

static void WriteReport(ReportWriter& report)
{
    report.Line(L"[Input]");
    report.Line(L"IAT hooks: %d", Hooks().InstalledCountForOwner("Input"));
    report.Line(L"DirectInput CreateDevice vtable hooked: %d", (int)g_createDeviceHooked);
    report.Line(L"DirectInput SetCooperativeLevel vtable hooked: %d", (int)g_setCoopHooked);
    report.Line(L"Focus-loss release thread started: %d", (int)g_focusThreadStarted);
    report.Line(L"");
}

static const Module kModule = {
    L"Input", RegisterConfig, ValidateConfig, Initialize,
    RegisterHooks, nullptr, Start, WriteReport
};
const Module& GetModule() { return kModule; }

} // namespace popopt::input

