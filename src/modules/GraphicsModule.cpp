#include "GraphicsModule.h"

namespace popopt::graphics {

static void RegisterConfig()
{
    Ini().AddSection(L"Graphics", L"Graphics");
    Ini().AddInt(L"Graphics", L"Quality", 2, {L"0=Low, 1=Medium, 2=High."});
    Ini().AddInt(L"Graphics", L"AntiAliasing", 4, {L"Valid values: 0, 2, 4, 8."});
    Ini().AddBool(L"Graphics", L"HighResolutionTextures", true,
                  {L"1=high-resolution textures, 0=degraded/low textures."});

    Ini().AddSection(L"AdvancedGraphics", L"Advanced Graphics");
    Ini().AddInt(L"AdvancedGraphics", L"ShadowQuality", 2);
    Ini().AddInt(L"AdvancedGraphics", L"PostEffects", 2);
}

static int NearestAA(int value)
{
    const int allowed[] = {0, 2, 4, 8};
    int best = allowed[0];
    int distance = abs(value - best);
    for (int candidate : allowed) {
        int current = abs(value - candidate);
        if (current < distance) {
            best = candidate;
            distance = current;
        }
    }
    return best;
}

static void ValidateConfig()
{
    Ini().ValidateRange("Graphics", "Quality", 2, 0, 2);
    int aa = Ini().GetInt("Graphics", "AntiAliasing", 4);
    int fixed = NearestAA(aa);
    if (aa != fixed) {
        Ini().SetInt("Graphics", "AntiAliasing", fixed);
        ++App().configCorrections;
        Log().Write(1, "Config corrected: [Graphics] AntiAliasing=%d -> %d", aa, fixed);
    }
    Ini().ValidateBool("Graphics", "HighResolutionTextures", 1);
    Ini().ValidateRange("AdvancedGraphics", "ShadowQuality", 2, 0, 2);
    Ini().ValidateRange("AdvancedGraphics", "PostEffects", 2, 0, 2);
}

static bool Initialize() { return true; }

static void WriteReport(ReportWriter& report)
{
    report.Line(L"[Graphics]");
    report.Line(L"Quality: %d", Ini().GetInt("Graphics", "Quality", 2));
    report.Line(L"AntiAliasing: %d", Ini().GetInt("Graphics", "AntiAliasing", 4));
    report.Line(L"HighResolutionTextures: %d", Ini().GetInt("Graphics", "HighResolutionTextures", 1));
    report.Line(L"ShadowQuality: %d", Ini().GetInt("AdvancedGraphics", "ShadowQuality", 2));
    report.Line(L"PostEffects: %d", Ini().GetInt("AdvancedGraphics", "PostEffects", 2));
    report.Line(L"");
}

static const Module kModule = {
    L"Graphics", RegisterConfig, ValidateConfig, Initialize,
    nullptr, nullptr, nullptr, WriteReport
};
const Module& GetModule() { return kModule; }

} // namespace popopt::graphics
