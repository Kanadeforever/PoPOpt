#include "CpuModule.h"

namespace popopt::cpu {

static int g_appliedCores = 0;
static DWORD_PTR g_appliedMask = 0;

static void RegisterConfig()
{
    Ini().AddSection(L"Performance", L"Performance");
    Ini().AddBool(L"Performance", L"LimitCpuCores", true,
                  {L"Limits this old game to a smaller logical-CPU set."});
    Ini().AddInt(L"Performance", L"MaxCpuCores", 4,
                 {L"Automatically clamped to 1..10; default 4."});
}

static void ValidateConfig()
{
    Ini().ValidateBool("Performance", "LimitCpuCores", 1);
    Ini().ValidateRange("Performance", "MaxCpuCores", 4, 1, 10);
}

static int CountBits(DWORD_PTR value)
{
    int count = 0;
    while (value) {
        count += (int)(value & 1);
        value >>= 1;
    }
    return count;
}

static DWORD_PTR FirstNMask(DWORD_PTR allowed, int count)
{
    DWORD_PTR result = 0;
    int selected = 0;
    for (int bit = 0; bit < (int)(sizeof(DWORD_PTR) * 8); ++bit) {
        DWORD_PTR mask = ((DWORD_PTR)1 << bit);
        if (!(allowed & mask)) continue;
        result |= mask;
        if (++selected >= count) break;
    }
    return result;
}

static bool Initialize()
{
    if (!Ini().GetInt("Performance", "LimitCpuCores", 1)) {
        Log().Write(2, "CPU affinity limiter disabled");
        return true;
    }

    const int requested = ClampInt(Ini().GetInt("Performance", "MaxCpuCores", 4), 1, 10);
    DWORD_PTR processMask = 0, systemMask = 0;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask)) {
        Log().Write(1, "CPU affinity: GetProcessAffinityMask failed, error=%lu", GetLastError());
        return true;
    }

    const DWORD_PTR allowed = processMask ? processMask : systemMask;
    const int available = CountBits(allowed);
    const int applied = std::min(requested, available);
    const DWORD_PTR newMask = FirstNMask(allowed, applied);
    if (!newMask) {
        Log().Write(1, "CPU affinity: computed mask is zero");
        return true;
    }

    if (newMask == processMask || SetProcessAffinityMask(GetCurrentProcess(), newMask)) {
        g_appliedCores = applied;
        g_appliedMask = newMask;
        Log().Write(2, "CPU affinity limited: requested=%d applied=%d available=%d mask=0x%p",
                    requested, applied, available, (void*)newMask);
    } else {
        Log().Write(1, "CPU affinity: SetProcessAffinityMask failed, error=%lu", GetLastError());
    }
    return true;
}

static void WriteReport(ReportWriter& report)
{
    report.Line(L"[Performance]");
    report.Line(L"CPU affinity cores applied: %d", g_appliedCores);
    report.Line(L"CPU affinity mask: 0x%p", (void*)g_appliedMask);
    report.Line(L"");
}

static const Module kModule = {
    L"Performance", RegisterConfig, ValidateConfig, Initialize,
    nullptr, nullptr, nullptr, WriteReport
};
const Module& GetModule() { return kModule; }

} // namespace popopt::cpu
