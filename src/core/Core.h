#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <wchar.h>
#include <wctype.h>
#include <stdlib.h>

#include <algorithm>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace popopt {

constexpr size_t kPathCapacity = MAX_PATH * 4;
constexpr const char* kLogPrefix = "[PoP_UniversalPatch] ";

enum class ExecutableFlavor {
    Unknown = 0,
    GogOrUnpackedLike = 1,
    SteamUnpacked = 2,
    SteamPackedStub = 3,
};

struct AppContext {
    HMODULE asiModule = nullptr;
    HMODULE gameModule = nullptr;

    wchar_t asiDir[kPathCapacity] = {};
    wchar_t gameDir[kPathCapacity] = {};
    wchar_t iniPath[kPathCapacity] = {};
    wchar_t logPath[kPathCapacity] = {};
    wchar_t reportPath[kPathCapacity] = {};

    int logLevel = 0;
    int configCorrections = 0;
    ExecutableFlavor exeFlavor = ExecutableFlavor::Unknown;
};

AppContext& App();

void WideToUtf8(const wchar_t* src, char* dst, int dstBytes);
void AnsiToWide(const char* src, wchar_t* dst, int dstCount);
void GetModuleDirectory(HMODULE module, wchar_t* out, DWORD count);
void InitializeFixedPaths(HMODULE asiModule);
int ClampInt(int value, int minimum, int maximum);

class Logger {
public:
    void Initialize(const wchar_t* path, int level);
    void SetLevel(int level);
    int Level() const;
    void ClearOldFile();

    void Write(int level, const char* format, ...);
    void WriteV(int level, const char* format, va_list args);

private:
    wchar_t path_[kPathCapacity] = {};
    int level_ = 0;
    SRWLOCK lock_ = SRWLOCK_INIT;
};

Logger& Log();

struct ConfigLine {
    enum class Type { Comment, Section, Integer, Boolean, String };
    Type type = Type::Comment;
    std::wstring section;
    std::wstring key;
    std::wstring value;
    std::vector<std::wstring> comments;
};

class Config {
public:
    void SetPath(const wchar_t* path);
    const wchar_t* Path() const;

    void AddHeaderComment(const wchar_t* text);
    void AddSection(const wchar_t* section, const wchar_t* title = nullptr);
    void AddInt(const wchar_t* section, const wchar_t* key, int defaultValue,
                std::initializer_list<const wchar_t*> comments = {});
    void AddBool(const wchar_t* section, const wchar_t* key, bool defaultValue,
                 std::initializer_list<const wchar_t*> comments = {});
    void AddString(const wchar_t* section, const wchar_t* key, const wchar_t* defaultValue,
                   std::initializer_list<const wchar_t*> comments = {});

    bool EnsureDefaultFile();

    int GetInt(const char* section, const char* key, int defaultValue) const;
    int GetIntW(const wchar_t* section, const wchar_t* key, int defaultValue) const;
    std::wstring GetString(const wchar_t* section, const wchar_t* key,
                           const wchar_t* defaultValue = L"") const;
    void SetInt(const char* section, const char* key, int value);
    void SetIntW(const wchar_t* section, const wchar_t* key, int value);

    int ValidateRange(const char* section, const char* key, int defaultValue,
                      int minimum, int maximum, const char* logLabel = nullptr);
    int ValidateBool(const char* section, const char* key, int defaultValue,
                     const char* logLabel = nullptr);
    int CorrectInt(const char* section, const char* key, int oldValue, int newValue,
                   const char* logLabel = nullptr);

private:
    wchar_t path_[kPathCapacity] = {};
    std::vector<ConfigLine> lines_;
};

Config& Ini();

class ReportWriter {
public:
    explicit ReportWriter(HANDLE file) : file_(file) {}
    void Line(const wchar_t* format, ...);
    HANDLE Handle() const { return file_; }

private:
    HANDLE file_ = INVALID_HANDLE_VALUE;
};

using DynamicProcResolver = FARPROC (*)(HMODULE module, LPCSTR name, FARPROC original);
using RuntimeEventCallback = void (*)(const char* reason);
using SimpleEventCallback = void (*)();

class HookManager {
public:
    void RegisterIatHook(std::initializer_list<const char*> dllNames,
                         const char* procName,
                         void* replacement,
                         void** originalSlot,
                         const char* owner);
    void RegisterDynamicResolver(DynamicProcResolver resolver);
    int InstallMainModuleHooks();
    int InstalledCountForOwner(const char* owner) const;

    static bool PatchVtableEntry(void** vtable, int index, void* replacement,
                                 void** originalSlot, const char* label);

private:
    struct IatSpec {
        std::vector<std::string> dllNames;
        std::string procName;
        void* replacement = nullptr;
        void** originalSlot = nullptr;
        std::string owner;
        int installed = 0;
    };

    std::vector<IatSpec> specs_;
    std::vector<DynamicProcResolver> dynamicResolvers_;

    static FARPROC WINAPI Hook_GetProcAddress(HMODULE module, LPCSTR name);
    static FARPROC DispatchDynamicResolvers(HMODULE module, LPCSTR name, FARPROC original);
    static HookManager* active_;
    static FARPROC (WINAPI *realGetProcAddress_)(HMODULE, LPCSTR);
};

HookManager& Hooks();

class EventBus {
public:
    void SubscribeLanguageRegistryQuery(RuntimeEventCallback callback);
    void NotifyLanguageRegistryQuery(const char* reason);
    void SubscribeReportRefresh(SimpleEventCallback callback);
    void RequestReportRefresh();

private:
    std::vector<RuntimeEventCallback> languageQueryCallbacks_;
    std::vector<SimpleEventCallback> reportRefreshCallbacks_;
};

EventBus& Events();

} // namespace popopt
