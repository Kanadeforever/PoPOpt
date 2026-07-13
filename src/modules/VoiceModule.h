#pragma once
#include "../core/Module.h"

namespace popopt::voice {
const Module& GetModule();
int RequestedLanguage();
int EffectiveLanguage();
bool RuntimeEnabled();
bool VoicePackExists(int language);
void BuildVoicePackPath(int language, wchar_t* out, int count);

// Synchronous language/voice bootstrap used from DllMain. Each function is
// idempotent so the regular module lifecycle may call the same phase later.
bool InitializeEarly();
void RegisterEarlyHooks();
void ApplyEarlyPatches();
}
