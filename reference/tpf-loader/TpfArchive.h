#pragma once

#include <windows.h>
#include <stdint.h>
#include <string>
#include <vector>

struct TpfTextureEntry {
    uint32_t hash = 0;
    std::string internalName;
    std::vector<unsigned char> imageData;
    uint32_t imageWidth = 0;
    uint32_t imageHeight = 0;
};

struct TpfLoadResult {
    bool ok = false;
    std::wstring error;
    std::vector<TpfTextureEntry> textures;
};

// Loads classic TexMod .tpf files or ordinary .zip packages containing texmod.def.
// Classic TPF handling:
//   1) XOR-decrypt the complete file with 0x3FA43FA4.
//   2) Open the resulting traditional-PKZIP-encrypted archive with TexMod's password.
//   3) Parse texmod.def and extract referenced image files.
TpfLoadResult LoadTpfOrZipPackage(const std::wstring& path);
