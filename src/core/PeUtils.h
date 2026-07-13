#pragma once
#include "Core.h"

namespace popopt::pe {

struct RvaCandidate {
    const char* label;
    DWORD rva;
};

struct PatchItem {
    const char* name = nullptr;
    DWORD rva = 0;
    const RvaCandidate* candidates = nullptr;
    int candidateCount = 0;
    const BYTE* expected = nullptr;
    const char* mask = nullptr;
    DWORD expectedSize = 0;
    const BYTE* patch = nullptr;
    DWORD patchSize = 0;
    bool allowFullScan = false;
    BYTE* target = nullptr;
    DWORD oldProtect = 0;
};

bool GetImageRange(HMODULE module, BYTE** baseOut, DWORD* sizeOut);
bool HasSectionNamed(const char* wanted);
DWORD GetEntryPointRva();
ExecutableFlavor DetectExecutableFlavor(bool logResult = true);
const wchar_t* FlavorName(ExecutableFlavor flavor);

BYTE* ResolvePatchTarget(const char* name, DWORD rva,
                         const BYTE* expected, DWORD expectedSize);
BYTE* ResolvePatchTargetCandidatesMasked(const char* name,
                                         const RvaCandidate* candidates,
                                         int candidateCount,
                                         const BYTE* expected,
                                         const char* mask,
                                         DWORD expectedSize,
                                         bool allowFullScan);
bool MatchMaskAt(const BYTE* data, const BYTE* pattern,
                 const char* mask, DWORD size);
bool WritePatch(const char* name, BYTE* destination,
                const BYTE* patch, DWORD patchSize);
bool PatchByRvaOrScan(const char* name, DWORD rva,
                      const BYTE* expected, DWORD expectedSize,
                      const BYTE* patch, DWORD patchSize);
bool PatchByCandidatesMasked(const char* name,
                             const RvaCandidate* candidates,
                             int candidateCount,
                             const BYTE* expected,
                             const char* mask,
                             DWORD expectedSize,
                             const BYTE* patch,
                             DWORD patchSize,
                             bool allowFullScan);
bool ApplyPatchGroupAtomic(const char* groupName, PatchItem* items, int count);

} // namespace popopt::pe
