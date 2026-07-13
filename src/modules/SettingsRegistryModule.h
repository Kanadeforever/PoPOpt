#pragma once
#include "../core/Module.h"

namespace popopt::settings {
const Module& GetModule();
int ReadTextLanguage();
DWORD ReadAspectRatioOverride();

// Synchronous bootstrap used from DllMain. Registration is idempotent, so the
// normal module pass may call RegisterHooks again without duplicating hooks.
void RegisterEarlyHooks();
LONG LanguageQueryCount();
LONG LastLanguageValue();
}
