#include "Core.h"

namespace popopt {

static AppContext g_app;
static Logger g_logger;
static Config g_config;
static HookManager g_hooks;
static EventBus g_events;

AppContext& App() { return g_app; }
Logger& Log() { return g_logger; }
Config& Ini() { return g_config; }
HookManager& Hooks() { return g_hooks; }
EventBus& Events() { return g_events; }

void WideToUtf8(const wchar_t* src, char* dst, int dstBytes)
{
    if (!dst || dstBytes <= 0) return;
    dst[0] = 0;
    if (!src) return;
    WideCharToMultiByte(CP_UTF8, 0, src, -1, dst, dstBytes, nullptr, nullptr);
    dst[dstBytes - 1] = 0;
}

void AnsiToWide(const char* src, wchar_t* dst, int dstCount)
{
    if (!dst || dstCount <= 0) return;
    dst[0] = 0;
    if (!src) return;
    MultiByteToWideChar(CP_ACP, 0, src, -1, dst, dstCount);
    dst[dstCount - 1] = 0;
}

void GetModuleDirectory(HMODULE module, wchar_t* out, DWORD count)
{
    if (!out || count == 0) return;
    out[0] = 0;
    GetModuleFileNameW(module, out, count);
    out[count - 1] = 0;
    wchar_t* slash = wcsrchr(out, L'\\');
    if (!slash) slash = wcsrchr(out, L'/');
    if (slash) *slash = 0;
}

void InitializeFixedPaths(HMODULE asiModule)
{
    g_app.asiModule = asiModule;
    g_app.gameModule = GetModuleHandleW(nullptr);
    GetModuleDirectory(asiModule, g_app.asiDir, (DWORD)kPathCapacity);
    GetModuleDirectory(nullptr, g_app.gameDir, (DWORD)kPathCapacity);
#if defined(_MSC_VER)
    _snwprintf_s(g_app.iniPath, kPathCapacity, _TRUNCATE,
                 L"%ls\\PoP_UniversalPatch.ini", g_app.asiDir);
    _snwprintf_s(g_app.logPath, kPathCapacity, _TRUNCATE,
                 L"%ls\\PoP_UniversalPatch.log", g_app.asiDir);
    _snwprintf_s(g_app.reportPath, kPathCapacity, _TRUNCATE,
                 L"%ls\\PoP_CompatibilityReport.txt", g_app.asiDir);
#else
    swprintf(g_app.iniPath, kPathCapacity, L"%ls\\PoP_UniversalPatch.ini", g_app.asiDir);
    swprintf(g_app.logPath, kPathCapacity, L"%ls\\PoP_UniversalPatch.log", g_app.asiDir);
    swprintf(g_app.reportPath, kPathCapacity, L"%ls\\PoP_CompatibilityReport.txt", g_app.asiDir);
#endif
    g_config.SetPath(g_app.iniPath);
}

int ClampInt(int value, int minimum, int maximum)
{
    return std::max(minimum, std::min(maximum, value));
}

void Logger::Initialize(const wchar_t* path, int level)
{
    if (path) {
#if defined(_MSC_VER)
        wcsncpy_s(path_, path, _TRUNCATE);
#else
        wcsncpy(path_, path, kPathCapacity - 1);
        path_[kPathCapacity - 1] = 0;
#endif
    }
    level_ = ClampInt(level, 0, 3);
}

void Logger::SetLevel(int level) { level_ = ClampInt(level, 0, 3); }
int Logger::Level() const { return level_; }

void Logger::ClearOldFile()
{
    if (path_[0]) DeleteFileW(path_);
}

void Logger::WriteV(int level, const char* format, va_list args)
{
    if (level_ < level || !path_[0]) return;

    char body[1800] = {};
#if defined(_MSC_VER)
    _vsnprintf_s(body, sizeof(body), _TRUNCATE, format, args);
#else
    vsnprintf(body, sizeof(body), format, args);
#endif
    body[sizeof(body) - 1] = 0;

    char line[2200] = {};
#if defined(_MSC_VER)
    _snprintf_s(line, sizeof(line), _TRUNCATE, "%s%s\r\n", kLogPrefix, body);
#else
    snprintf(line, sizeof(line), "%s%s\r\n", kLogPrefix, body);
#endif

    AcquireSRWLockExclusive(&lock_);
    HANDLE file = CreateFileW(path_, FILE_APPEND_DATA,
                              FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(file, line, (DWORD)strlen(line), &written, nullptr);
        CloseHandle(file);
    }
    ReleaseSRWLockExclusive(&lock_);

    OutputDebugStringA(line);
}

void Logger::Write(int level, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    WriteV(level, format, args);
    va_end(args);
}

void Config::SetPath(const wchar_t* path)
{
    if (!path) return;
#if defined(_MSC_VER)
    wcsncpy_s(path_, path, _TRUNCATE);
#else
    wcsncpy(path_, path, kPathCapacity - 1);
    path_[kPathCapacity - 1] = 0;
#endif
}

const wchar_t* Config::Path() const { return path_; }

void Config::AddHeaderComment(const wchar_t* text)
{
    ConfigLine line;
    line.type = ConfigLine::Type::Comment;
    line.value = text ? text : L"";
    lines_.push_back(std::move(line));
}

void Config::AddSection(const wchar_t* section, const wchar_t* title)
{
    ConfigLine line;
    line.type = ConfigLine::Type::Section;
    line.section = section ? section : L"";
    if (title && *title) line.comments.push_back(title);
    lines_.push_back(std::move(line));
}

static std::vector<std::wstring> MakeComments(std::initializer_list<const wchar_t*> comments)
{
    std::vector<std::wstring> out;
    for (const wchar_t* comment : comments) {
        if (comment) out.emplace_back(comment);
    }
    return out;
}

void Config::AddInt(const wchar_t* section, const wchar_t* key, int defaultValue,
                    std::initializer_list<const wchar_t*> comments)
{
    ConfigLine line;
    line.type = ConfigLine::Type::Integer;
    line.section = section ? section : L"";
    line.key = key ? key : L"";
    line.value = std::to_wstring(defaultValue);
    line.comments = MakeComments(comments);
    lines_.push_back(std::move(line));
}

void Config::AddBool(const wchar_t* section, const wchar_t* key, bool defaultValue,
                     std::initializer_list<const wchar_t*> comments)
{
    ConfigLine line;
    line.type = ConfigLine::Type::Boolean;
    line.section = section ? section : L"";
    line.key = key ? key : L"";
    line.value = defaultValue ? L"1" : L"0";
    line.comments = MakeComments(comments);
    lines_.push_back(std::move(line));
}

void Config::AddString(const wchar_t* section, const wchar_t* key,
                       const wchar_t* defaultValue,
                       std::initializer_list<const wchar_t*> comments)
{
    ConfigLine line;
    line.type = ConfigLine::Type::String;
    line.section = section ? section : L"";
    line.key = key ? key : L"";
    line.value = defaultValue ? defaultValue : L"";
    line.comments = MakeComments(comments);
    lines_.push_back(std::move(line));
}

bool Config::EnsureDefaultFile()
{
    if (!path_[0]) return false;
    if (GetFileAttributesW(path_) != INVALID_FILE_ATTRIBUTES) return true;

    HANDLE file = CreateFileW(path_, GENERIC_WRITE, 0, nullptr,
                              CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;

    const WORD bom = 0xFEFF;
    DWORD written = 0;
    WriteFile(file, &bom, sizeof(bom), &written, nullptr);

    auto writeText = [&](const std::wstring& text) {
        if (!text.empty()) {
            WriteFile(file, text.data(), (DWORD)(text.size() * sizeof(wchar_t)), &written, nullptr);
        }
    };

    std::wstring currentSection;
    for (const ConfigLine& line : lines_) {
        if (line.type == ConfigLine::Type::Comment) {
            writeText(L"; " + line.value + L"\r\n");
            continue;
        }
        if (line.type == ConfigLine::Type::Section) {
            writeText(L"\r\n");
            for (const std::wstring& comment : line.comments)
                writeText(L"; -------------------- " + comment + L" --------------------\r\n");
            writeText(L"[" + line.section + L"]\r\n");
            currentSection = line.section;
            continue;
        }
        for (const std::wstring& comment : line.comments)
            writeText(L"; " + comment + L"\r\n");
        writeText(line.key + L"=" + line.value + L"\r\n");
    }

    CloseHandle(file);
    return true;
}

int Config::GetInt(const char* section, const char* key, int defaultValue) const
{
    wchar_t ws[128] = {}, wk[128] = {};
    AnsiToWide(section, ws, 128);
    AnsiToWide(key, wk, 128);
    return GetPrivateProfileIntW(ws, wk, defaultValue, path_);
}

int Config::GetIntW(const wchar_t* section, const wchar_t* key, int defaultValue) const
{
    return GetPrivateProfileIntW(section, key, defaultValue, path_);
}

std::wstring Config::GetString(const wchar_t* section, const wchar_t* key,
                               const wchar_t* defaultValue) const
{
    wchar_t buffer[kPathCapacity] = {};
    GetPrivateProfileStringW(section, key, defaultValue ? defaultValue : L"",
                             buffer, (DWORD)kPathCapacity, path_);
    return buffer;
}

void Config::SetInt(const char* section, const char* key, int value)
{
    wchar_t ws[128] = {}, wk[128] = {};
    AnsiToWide(section, ws, 128);
    AnsiToWide(key, wk, 128);
    SetIntW(ws, wk, value);
}

void Config::SetIntW(const wchar_t* section, const wchar_t* key, int value)
{
    wchar_t number[64] = {};
    wsprintfW(number, L"%d", value);
    WritePrivateProfileStringW(section, key, number, path_);
}

int Config::CorrectInt(const char* section, const char* key,
                       int oldValue, int newValue, const char* logLabel)
{
    if (oldValue == newValue) return newValue;
    SetInt(section, key, newValue);
    ++App().configCorrections;
    Log().Write(1, "Config corrected: [%s] %s=%d -> %d%s%s%s",
                section, key, oldValue, newValue,
                logLabel ? " (" : "", logLabel ? logLabel : "", logLabel ? ")" : "");
    return newValue;
}

int Config::ValidateRange(const char* section, const char* key, int defaultValue,
                          int minimum, int maximum, const char* logLabel)
{
    int value = GetInt(section, key, defaultValue);
    int fixed = ClampInt(value, minimum, maximum);
    if (value != fixed) {
        SetInt(section, key, fixed);
        ++App().configCorrections;
        Log().Write(1, "Config corrected: [%s] %s=%d -> %d%s%s%s",
                    section, key, value, fixed,
                    logLabel ? " (" : "", logLabel ? logLabel : "", logLabel ? ")" : "");
    }
    return fixed;
}

int Config::ValidateBool(const char* section, const char* key, int defaultValue,
                         const char* logLabel)
{
    int value = GetInt(section, key, defaultValue);
    int fixed = value ? 1 : 0;
    if (value != fixed) {
        SetInt(section, key, fixed);
        ++App().configCorrections;
        Log().Write(1, "Config corrected: [%s] %s=%d -> %d%s%s%s",
                    section, key, value, fixed,
                    logLabel ? " (" : "", logLabel ? logLabel : "", logLabel ? ")" : "");
    }
    return fixed;
}

void ReportWriter::Line(const wchar_t* format, ...)
{
    if (file_ == INVALID_HANDLE_VALUE) return;
    wchar_t line[2048] = {};
    va_list args;
    va_start(args, format);
#if defined(_MSC_VER)
    _vsnwprintf_s(line, _countof(line), _TRUNCATE, format, args);
#else
    vswprintf(line, sizeof(line) / sizeof(line[0]), format, args);
#endif
    va_end(args);
    line[_countof(line) - 1] = 0;
    DWORD written = 0;
    WriteFile(file_, line, (DWORD)(wcslen(line) * sizeof(wchar_t)), &written, nullptr);
    WriteFile(file_, L"\r\n", 4, &written, nullptr);
}

HookManager* HookManager::active_ = nullptr;
FARPROC (WINAPI *HookManager::realGetProcAddress_)(HMODULE, LPCSTR) = nullptr;

void HookManager::RegisterIatHook(std::initializer_list<const char*> dllNames,
                                  const char* procName,
                                  void* replacement,
                                  void** originalSlot,
                                  const char* owner)
{
    IatSpec spec;
    for (const char* dll : dllNames) {
        if (dll) spec.dllNames.emplace_back(dll);
    }
    spec.procName = procName ? procName : "";
    spec.replacement = replacement;
    spec.originalSlot = originalSlot;
    spec.owner = owner ? owner : "Core";
    specs_.push_back(std::move(spec));
}

void HookManager::RegisterDynamicResolver(DynamicProcResolver resolver)
{
    if (!resolver) return;
    dynamicResolvers_.push_back(resolver);
}

static bool EqualsInsensitive(const char* a, const std::string& b)
{
    return a && _stricmp(a, b.c_str()) == 0;
}

int HookManager::InstallMainModuleHooks()
{
    active_ = this;
    if (!dynamicResolvers_.empty()) {
        RegisterIatHook({"KERNEL32.DLL", "KERNELBASE.DLL"}, "GetProcAddress",
                        (void*)Hook_GetProcAddress,
                        (void**)&realGetProcAddress_, "HookManager");
    }

    HMODULE module = GetModuleHandleW(nullptr);
    if (!module) return 0;
    BYTE* base = (BYTE*)module;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    DWORD importRva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (!importRva) return 0;

    int total = 0;
    IMAGE_IMPORT_DESCRIPTOR* descriptor = (IMAGE_IMPORT_DESCRIPTOR*)(base + importRva);
    for (; descriptor->Name; ++descriptor) {
        const char* importedDll = (const char*)(base + descriptor->Name);
        IMAGE_THUNK_DATA* names = descriptor->OriginalFirstThunk
            ? (IMAGE_THUNK_DATA*)(base + descriptor->OriginalFirstThunk)
            : (IMAGE_THUNK_DATA*)(base + descriptor->FirstThunk);
        IMAGE_THUNK_DATA* iat = (IMAGE_THUNK_DATA*)(base + descriptor->FirstThunk);
        if (!names || !iat) continue;

        for (int index = 0; names[index].u1.AddressOfData; ++index) {
            if (IMAGE_SNAP_BY_ORDINAL(names[index].u1.Ordinal)) continue;
            IMAGE_IMPORT_BY_NAME* importName =
                (IMAGE_IMPORT_BY_NAME*)(base + names[index].u1.AddressOfData);
            const char* procName = (const char*)importName->Name;
            if (!procName) continue;

            for (IatSpec& spec : specs_) {
                bool dllMatched = false;
                for (const std::string& candidate : spec.dllNames) {
                    if (EqualsInsensitive(importedDll, candidate)) {
                        dllMatched = true;
                        break;
                    }
                }
                if (!dllMatched || _stricmp(procName, spec.procName.c_str()) != 0) continue;

                void** slot = (void**)&iat[index].u1.Function;
                if (spec.originalSlot && !*spec.originalSlot) *spec.originalSlot = *slot;
                if (*slot == spec.replacement) continue;

                DWORD oldProtect = 0;
                if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
                    Log().Write(1, "%s IAT hook failed for %s!%s, error=%lu",
                                spec.owner.c_str(), importedDll, procName, GetLastError());
                    continue;
                }
                *slot = spec.replacement;
                DWORD ignored = 0;
                VirtualProtect(slot, sizeof(void*), oldProtect, &ignored);
                FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));
                ++spec.installed;
                ++total;
            }
        }
    }

    std::map<std::string, int> ownerCounts;
    for (const IatSpec& spec : specs_) ownerCounts[spec.owner] += spec.installed;
    for (const auto& pair : ownerCounts)
        Log().Write(2, "%s IAT hooks installed: %d", pair.first.c_str(), pair.second);
    return total;
}

int HookManager::InstalledCountForOwner(const char* owner) const
{
    int count = 0;
    for (const IatSpec& spec : specs_) {
        if (owner && _stricmp(owner, spec.owner.c_str()) == 0) count += spec.installed;
    }
    return count;
}

FARPROC HookManager::DispatchDynamicResolvers(HMODULE module, LPCSTR name, FARPROC original)
{
    if (!active_) return original;
    FARPROC result = original;
    for (DynamicProcResolver resolver : active_->dynamicResolvers_) {
        FARPROC candidate = resolver(module, name, result);
        if (candidate) result = candidate;
    }
    return result;
}

FARPROC WINAPI HookManager::Hook_GetProcAddress(HMODULE module, LPCSTR name)
{
    FARPROC original = realGetProcAddress_ ? realGetProcAddress_(module, name) : nullptr;
    if (!name || (ULONG_PTR)name <= 0xFFFF || !original) return original;
    return DispatchDynamicResolvers(module, name, original);
}

bool HookManager::PatchVtableEntry(void** vtable, int index, void* replacement,
                                   void** originalSlot, const char* label)
{
    if (!vtable || !replacement || !originalSlot) return false;
    void** slot = &vtable[index];
    void* current = *slot;
    if (current == replacement) return true;
    if (*originalSlot && current != *originalSlot) {
        Log().Write(1, "%s vtable slot already replaced by another module; skipped",
                    label ? label : "vtable");
        return false;
    }

    DWORD oldProtect = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        Log().Write(1, "%s VirtualProtect failed, error=%lu",
                    label ? label : "vtable", GetLastError());
        return false;
    }
    if (!*originalSlot) *originalSlot = current;
    *slot = replacement;
    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(void*), oldProtect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));
    return true;
}

void EventBus::SubscribeLanguageRegistryQuery(RuntimeEventCallback callback)
{
    if (callback) languageQueryCallbacks_.push_back(callback);
}

void EventBus::NotifyLanguageRegistryQuery(const char* reason)
{
    for (RuntimeEventCallback callback : languageQueryCallbacks_) callback(reason);
}

void EventBus::SubscribeReportRefresh(SimpleEventCallback callback)
{
    if (callback) reportRefreshCallbacks_.push_back(callback);
}

void EventBus::RequestReportRefresh()
{
    for (SimpleEventCallback callback : reportRefreshCallbacks_) callback();
}

} // namespace popopt
