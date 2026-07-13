#pragma once
#include "../core/Module.h"

namespace popopt::diagnostics {
const Module& GetModule();
void SetModuleList(const Module* const* modules, size_t count);
void WriteNow();
}
