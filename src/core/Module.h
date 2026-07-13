#pragma once
#include "Core.h"

namespace popopt {

struct Module {
    const wchar_t* name = L"Unnamed";
    void (*RegisterConfig)() = nullptr;
    void (*ValidateConfig)() = nullptr;
    bool (*Initialize)() = nullptr;
    void (*RegisterHooks)() = nullptr;
    void (*ApplyPatches)() = nullptr;
    void (*Start)() = nullptr;
    void (*WriteReport)(ReportWriter&) = nullptr;
};

} // namespace popopt
