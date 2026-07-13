#include "PeUtils.h"

namespace popopt::pe {

bool GetImageRange(HMODULE module, BYTE** baseOut, DWORD* sizeOut)
{
    if (!module || !baseOut || !sizeOut) return false;
    BYTE* base = (BYTE*)module;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    *baseOut = base;
    *sizeOut = nt->OptionalHeader.SizeOfImage;
    return true;
}

static BYTE* FindBytes(BYTE* base, DWORD size, const BYTE* pattern, DWORD patternSize)
{
    if (!base || !pattern || !patternSize || size < patternSize) return nullptr;
    for (DWORD i = 0; i <= size - patternSize; ++i) {
        if (memcmp(base + i, pattern, patternSize) == 0) return base + i;
    }
    return nullptr;
}

bool MatchMaskAt(const BYTE* data, const BYTE* pattern, const char* mask, DWORD size)
{
    if (!data || !pattern || !mask) return false;
    for (DWORD i = 0; i < size; ++i) {
        if (mask[i] == 'x' && data[i] != pattern[i]) return false;
    }
    return true;
}

static BYTE* FindBytesMasked(BYTE* base, DWORD size, const BYTE* pattern,
                             const char* mask, DWORD patternSize)
{
    if (!base || !pattern || !mask || !patternSize || size < patternSize) return nullptr;
    for (DWORD i = 0; i <= size - patternSize; ++i) {
        if (MatchMaskAt(base + i, pattern, mask, patternSize)) return base + i;
    }
    return nullptr;
}

bool HasSectionNamed(const char* wanted)
{
    HMODULE module = GetModuleHandleW(nullptr);
    if (!module || !wanted) return false;
    BYTE* base = (BYTE*)module;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

    IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        char name[9] = {};
        memcpy(name, section[i].Name, 8);
        if (_stricmp(name, wanted) == 0) return true;
    }
    return false;
}

DWORD GetEntryPointRva()
{
    HMODULE module = GetModuleHandleW(nullptr);
    if (!module) return 0;
    BYTE* base = (BYTE*)module;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    return nt->OptionalHeader.AddressOfEntryPoint;
}

ExecutableFlavor DetectExecutableFlavor(bool logResult)
{
    ExecutableFlavor flavor = ExecutableFlavor::GogOrUnpackedLike;
    if (HasSectionNamed(".bind")) flavor = ExecutableFlavor::SteamPackedStub;
    else if (HasSectionNamed(".extra")) flavor = ExecutableFlavor::SteamUnpacked;

    App().exeFlavor = flavor;
    if (logResult) {
        const DWORD entry = GetEntryPointRva();
        if (flavor == ExecutableFlavor::SteamPackedStub)
            Log().Write(2, "executable flavor: packed SteamStub detected (entry RVA 0x%08X)", (unsigned)entry);
        else if (flavor == ExecutableFlavor::SteamUnpacked)
            Log().Write(2, "executable flavor: Steam unpacked detected (entry RVA 0x%08X)", (unsigned)entry);
        else
            Log().Write(2, "executable flavor: GOG/unpacked-like detected (entry RVA 0x%08X)", (unsigned)entry);
    }
    return flavor;
}

const wchar_t* FlavorName(ExecutableFlavor flavor)
{
    switch (flavor) {
    case ExecutableFlavor::SteamPackedStub: return L"Steam packed / SteamStub";
    case ExecutableFlavor::SteamUnpacked: return L"Steam unpacked";
    case ExecutableFlavor::GogOrUnpackedLike: return L"GOG / unpacked-like";
    default: return L"Unknown / not evaluated";
    }
}

BYTE* ResolvePatchTarget(const char* name, DWORD rva,
                         const BYTE* expected, DWORD expectedSize)
{
    HMODULE module = GetModuleHandleW(nullptr);
    BYTE* base = nullptr;
    DWORD imageSize = 0;
    if (!module || !GetImageRange(module, &base, &imageSize)) return nullptr;

    if (rva + expectedSize <= imageSize) {
        BYTE* atRva = base + rva;
        if (memcmp(atRva, expected, expectedSize) == 0) return atRva;
    }

    BYTE* scanned = FindBytes(base, imageSize, expected, expectedSize);
    if (scanned) {
        Log().Write(2, "%s: RVA mismatch, pattern found at RVA 0x%08X",
                    name, (unsigned)(scanned - base));
        return scanned;
    }

    Log().Write(2, "%s: expected bytes not found", name);
    return nullptr;
}

BYTE* ResolvePatchTargetCandidatesMasked(const char* name,
                                         const RvaCandidate* candidates,
                                         int candidateCount,
                                         const BYTE* expected,
                                         const char* mask,
                                         DWORD expectedSize,
                                         bool allowFullScan)
{
    HMODULE module = GetModuleHandleW(nullptr);
    BYTE* base = nullptr;
    DWORD imageSize = 0;
    if (!module || !GetImageRange(module, &base, &imageSize)) return nullptr;

    for (int i = 0; i < candidateCount; ++i) {
        DWORD rva = candidates[i].rva;
        if (rva + expectedSize <= imageSize) {
            BYTE* atRva = base + rva;
            if (MatchMaskAt(atRva, expected, mask, expectedSize)) {
                Log().Write(2, "%s: matched %s RVA 0x%08X",
                            name, candidates[i].label, (unsigned)rva);
                return atRva;
            }
        }
    }

    if (allowFullScan) {
        BYTE* scanned = FindBytesMasked(base, imageSize, expected, mask, expectedSize);
        if (scanned) {
            Log().Write(2, "%s: candidate mismatch, wildcard pattern found at RVA 0x%08X",
                        name, (unsigned)(scanned - base));
            return scanned;
        }
    }

    Log().Write(2, "%s: expected bytes not found in supported candidates", name);
    return nullptr;
}

bool WritePatch(const char* name, BYTE* destination,
                const BYTE* patch, DWORD patchSize)
{
    if (!destination || !patch || !patchSize) return false;
    DWORD oldProtect = 0;
    if (!VirtualProtect(destination, patchSize, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        Log().Write(1, "%s: VirtualProtect failed, error=%lu", name, GetLastError());
        return false;
    }
    memcpy(destination, patch, patchSize);
    DWORD ignored = 0;
    VirtualProtect(destination, patchSize, oldProtect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), destination, patchSize);

    BYTE* base = (BYTE*)GetModuleHandleW(nullptr);
    Log().Write(2, "%s: patched at RVA 0x%08X",
                name, (unsigned)(destination - base));
    return true;
}

bool PatchByRvaOrScan(const char* name, DWORD rva,
                      const BYTE* expected, DWORD expectedSize,
                      const BYTE* patch, DWORD patchSize)
{
    BYTE* destination = ResolvePatchTarget(name, rva, expected, expectedSize);
    if (!destination) {
        Log().Write(2, "%s: patch skipped", name);
        return false;
    }
    return WritePatch(name, destination, patch, patchSize);
}

bool PatchByCandidatesMasked(const char* name,
                             const RvaCandidate* candidates,
                             int candidateCount,
                             const BYTE* expected,
                             const char* mask,
                             DWORD expectedSize,
                             const BYTE* patch,
                             DWORD patchSize,
                             bool allowFullScan)
{
    BYTE* destination = ResolvePatchTargetCandidatesMasked(
        name, candidates, candidateCount, expected, mask, expectedSize, allowFullScan);
    if (!destination) {
        Log().Write(2, "%s: patch skipped", name);
        return false;
    }
    return WritePatch(name, destination, patch, patchSize);
}

bool ApplyPatchGroupAtomic(const char* groupName, PatchItem* items, int count)
{
    if (!items || count <= 0) return false;
    BYTE* base = (BYTE*)GetModuleHandleW(nullptr);

    for (int i = 0; i < count; ++i) {
        if (items[i].candidates && items[i].candidateCount > 0 && items[i].mask) {
            items[i].target = ResolvePatchTargetCandidatesMasked(
                items[i].name, items[i].candidates, items[i].candidateCount,
                items[i].expected, items[i].mask, items[i].expectedSize,
                items[i].allowFullScan);
        } else {
            items[i].target = ResolvePatchTarget(
                items[i].name, items[i].rva, items[i].expected, items[i].expectedSize);
        }
        items[i].oldProtect = 0;
        if (!items[i].target) {
            Log().Write(2, "%s: atomic group skipped because %s did not match",
                        groupName, items[i].name);
            return false;
        }
    }

    for (int i = 0; i < count; ++i) {
        if (!VirtualProtect(items[i].target, items[i].patchSize,
                            PAGE_EXECUTE_READWRITE, &items[i].oldProtect)) {
            Log().Write(1, "%s: VirtualProtect failed on %s, error=%lu",
                        groupName, items[i].name, GetLastError());
            for (int j = 0; j < i; ++j) {
                DWORD ignored = 0;
                VirtualProtect(items[j].target, items[j].patchSize,
                               items[j].oldProtect, &ignored);
            }
            return false;
        }
    }

    for (int i = 0; i < count; ++i)
        memcpy(items[i].target, items[i].patch, items[i].patchSize);

    for (int i = 0; i < count; ++i) {
        DWORD ignored = 0;
        VirtualProtect(items[i].target, items[i].patchSize,
                       items[i].oldProtect, &ignored);
        FlushInstructionCache(GetCurrentProcess(), items[i].target, items[i].patchSize);
        Log().Write(2, "%s: patched at RVA 0x%08X",
                    items[i].name, (unsigned)(items[i].target - base));
    }
    Log().Write(2, "%s: atomic group applied", groupName);
    return true;
}

} // namespace popopt::pe
