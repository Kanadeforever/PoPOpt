# PoPOpt

[中文](README.md) | English

> Current development branch: v29 modular. Early text/voice initialization and the integrated TPF loader have been verified in-game.

## Overview

**PoPOpt (PoP Universal Patch)** is a single-file, 32-bit ASI patch for the PC version of *Prince of Persia (2008)*. It applies its changes at runtime and does not modify the game executable on disk.

The patch automatically detects and supports:

- GOG / unpacked builds
- Unpacked Steam builds
- Original Steam builds protected by SteamStub

Its main features include independent text and voice language selection, voice-pack detection and safe fallback, windowed and borderless modes, high-DPI awareness, external graphics settings, CPU affinity control, input fixes for windowed modes, configuration validation, logging, and compatibility reports. Version 29 also includes an optional TexMod TPF texture loader for controller prompts and other texture-replacement packages.

> This is an unofficial community patch. Keep a backup of the original game files and use a 32-bit ASI Loader compatible with the game.

## Features

- Independent text, menu, subtitle, and dialogue voice languages.
- Automatic detection of GOG, unpacked Steam, and SteamStub-protected Steam executables.
- Transactional four-point voice patching: Package + Resource + Bundle + Name.
- Voice-pack detection at startup with safe fallback to another language.
- Standard windowed mode and borderless window mode at a user-defined resolution.
- External resolution, aspect ratio, VSync, and widescreen settings.
- External anti-aliasing, overall quality, high-resolution textures, shadows, and post-effects settings.
- System, Per-Monitor, and Per-Monitor V2 DPI awareness.
- Logical CPU core limiting, with a default of 4 and a maximum of 10.
- Restores the Windows key in windowed and borderless modes.
- Releases the mouse when the game loses focus without interfering with normal mouse capture while focused.
- Automatic configuration-file generation, validation, and correction of invalid values.
- Configurable log levels and a compatibility report.
- Unicode path support for the configuration file, log, report, and voice-pack detection.
- Optional built-in TPF texture loading without `TexMod.exe`, `tmldr.dll`, `tmrls.dll`, or registry-based package passing.
- Modular source layout while still producing a single ASI module.

## Installation

1. Prepare a game setup capable of loading 32-bit ASI plugins.
2. Place `PoP_UniversalPatch.asi` where your ASI Loader can load it.
3. Place `PoP_UniversalPatch.ini` in the same directory as the ASI file.
4. Make sure the required `DataPC_StreamedSoundsXXX.forge` files are located in the same directory as the game executable.
5. To use texture replacement, place the TPF package in the ASI directory and list it under `[TexturePackages]`.
6. Start the game. The patch will detect the executable version, validate the configuration, and apply the appropriate compatibility fixes.

If `PoP_UniversalPatch.ini` does not exist, the patch automatically creates a complete UTF-16 default configuration in the ASI directory.

## Default Files

The patch uses the following fixed file names:

- `PoP_UniversalPatch.asi` — patch module
- `PoP_UniversalPatch.ini` — configuration file
- `PoP_UniversalPatch.log` — runtime log
- `PoP_CompatibilityReport.txt` — compatibility report


## TPF Texture Loader

Version 29 includes a standalone TexMod-compatible TPF module. It reads classic `.tpf` packages and their `texmod.def` files directly, without launching `TexMod.exe` and without passing package lists through `HKCU\SOFTWARE\TexMod`.

```ini
[TextureLoader]
Enable=1
LaterPackageWins=1

[TexturePackages]
Package1=XBOX.tpf
Package2=
Package3=
```

Behavior:

- Only packages explicitly listed under `[TexturePackages]` are loaded.
- Relative package paths are resolved from the ASI directory.
- With `LaterPackageWins=1`, later `PackageN` entries replace earlier definitions that use the same texture hash. Set it to `0` to keep the first definition.
- Matching uses TexMod-compatible texture hashes only. The replacement image does not have to use the same dimensions as the original texture.
- The verified game path uses the `top-compact-complement` hash mode. The tested Xbox prompt package matched and replaced all 10 target textures.
- With `Enable=0`, the module does not parse TPF files, resolve D3DX, or install D3D9 texture hooks.

The texture module resolves an available `d3dx9_*.dll` dynamically to decode replacement images. D3DX is not linked as a hard ASI dependency. If the log reports that no image loader was found, install the Microsoft DirectX End-User Runtimes (June 2010) or provide a compatible D3DX9 DLL with the game.

## Language IDs and Voice Packs

| ID | Language | Voice-pack suffix |
|---:|---|---|
| 0 | None / Auto | Available only for `TextLanguage` |
| 1 | English | Eng |
| 2 | French | Fre |
| 3 | Spanish | Spa |
| 4 | Polish | Pol |
| 5 | German | Ger |
| 6 | Chinese | Chi |
| 7 | Hungarian | Hun |
| 8 | Italian | Ita |
| 9 | Japanese | Jap |
| 10 | Czech | Cze |
| 11 | Korean | Kor |
| 12 | Russian | Rus |
| 13 | Dutch | Dut |

Voice packs use the following naming format:

```text
DataPC_StreamedSounds<Suffix>.forge
```

For example, the English voice pack is:

```text
DataPC_StreamedSoundsEng.forge
```

`TextLanguage` accepts values from `0` to `13`. `VoiceLanguage` and `FallbackLanguage` accept values from `1` to `13` only.

### Automatic Voice-Pack Detection and Safe Fallback

When `AutoFallback=1`:

1. The patch checks whether the voice pack selected by `VoiceLanguage` exists.
2. If the requested voice pack is missing, it checks `FallbackLanguage`.
3. If the fallback voice pack exists, the patch safely uses it for the current session.
4. If both the requested and fallback voice packs are missing, the entire voice patch is disabled to avoid an inconsistent state.

Automatic fallback changes only the effective voice language for the current session. It does not overwrite `[Language] VoiceLanguage` in the configuration file.

Keep the following options enabled when using full voice-language patching:

```ini
AssetPatch=1
RequireAssetPatch=1
```

With these settings, the Package, Resource, Bundle, and Name patch points must all match before any of them are written.

## Display Settings

Default display configuration:

```ini
[Display]
WindowMode=2
BorderlessCenter=0
Width=1920
Height=1080
VSync=1
Widescreen=1
AspectRatio=0
```

`WindowMode` values:

- `0` — Fullscreen
- `1` — Standard windowed mode
- `2` — Borderless window, default; uses the configured `Width × Height` and is not borderless fullscreen

`BorderlessCenter` applies only to borderless mode:

- `0` — Keep the game window position
- `1` — Center the window in the current monitor's work area

Valid resolution ranges:

- `Width`: 320 to 16384
- `Height`: 240 to 16384

When `AspectRatio=0`, the patch calculates the value automatically:

```text
floor((Width / Height) × 100)
```

Examples:

- 1920×1080 → 177
- 3840×2160 → 177
- 3440×1440 → 238

For a manually specified non-zero value, the valid range is 50 to 500.

## DPI Settings

```ini
[DPI]
DPIAware=1
DPIAwareness=1
```

`DPIAwareness` values:

- `1` — System DPI Aware
- `2` — Per-Monitor DPI Aware
- `3` — Per-Monitor V2

Newer Per-Monitor modes may change the window size or scaling behavior of this older game. Use mode `1` if you experience display-size problems.

## Graphics Settings

```ini
[Graphics]
Quality=2
AntiAliasing=4
HighResolutionTextures=1

[AdvancedGraphics]
ShadowQuality=2
PostEffects=2
```

Available values:

- `Quality`: `0=Low`, `1=Medium`, `2=High`
- `AntiAliasing`: `0=Off`, `2=x2`, `4=x4`, `8=x8`
- `HighResolutionTextures`: `0=Low-resolution textures`, `1=High-resolution textures`
- `ShadowQuality`: 0 to 2
- `PostEffects`: 0 to 2

Invalid anti-aliasing values are corrected to the nearest valid value: `0`, `2`, `4`, or `8`.

## Performance and Input

```ini
[Performance]
LimitCpuCores=1
MaxCpuCores=4

[Input]
AllowWinKeyInWindowed=1
ReleaseMouseOnFocusLoss=1
```

### CPU Affinity

When `LimitCpuCores=1`, the patch selects the first N logical processors from the affinity mask currently allowed by the operating system.

`MaxCpuCores` accepts values from 1 to 10 and defaults to 4. The patch never expands an affinity restriction already applied by Windows or another utility.

### Windows Key Fix

When `AllowWinKeyInWindowed=1`, the patch removes the DirectInput `DISCL_NOWINKEY` flag in standard windowed and borderless modes.

Fullscreen mode is not modified and keeps the game's original behavior.

### Mouse Release on Focus Loss

When `ReleaseMouseOnFocusLoss=1`, the game can capture the mouse normally while focused. After Alt+Tab or switching to another application, the patch releases `ClipCursor`.

This feature is active only in standard windowed and borderless modes.

## Configuration Validation and Automatic Correction

At startup, the patch validates the main configuration values:

- Boolean values are normalized to `0` or `1`.
- Language IDs are limited to their valid ranges.
- `WindowMode` is limited to 0 through 2.
- Resolution values are limited to safe ranges.
- Non-positive `AspectRatio` values become 0; positive values are limited to 50 through 500.
- `MaxCpuCores` is limited to 1 through 10.
- Anti-aliasing is corrected to the nearest supported value.
- Graphics, DPI, logging, and Steam runtime parameters are limited to their valid ranges.

If the entire configuration file is missing, the patch generates a complete default file. If the file already exists, only invalid or out-of-range values that are detected are corrected and written back; missing entries are not automatically added.

## Logging and Compatibility Report

```ini
[Debug]
LogLevel=2
CompatibilityReport=1
```

`LogLevel` values:

- `0` — Disable logging completely and delete the previous log
- `1` — Record warnings, errors, and configuration corrections only
- `2` — Record normal startup, detection, and patch status information
- `3` — Detailed debugging, including DataPC, forge, and sound-related file-open tracing

The report also records whether the texture module is enabled, the number of packages and unique hashes loaded, and the number of successful replacements.

When `CompatibilityReport=1`, the patch generates:

```text
PoP_CompatibilityReport.txt
```

The report includes:

- Game executable path, size, entry point, and detected build type
- Configuration-file path and the number of corrections made during the current run
- Requested and effective text and voice languages
- Window mode, resolution, aspect ratio, and CPU affinity
- Voice-patch, window-patch, and hook status
- All detected voice packs

### Advanced SteamStub Settings

```ini
[Debug]
PackedSteamPatchTries=10000
PackedSteamPatchIntervalMs=1
PackedSteamPatchStatusEvery=1000
```

These values control retry behavior for runtime voice patching on SteamStub-protected builds. Most users should leave them unchanged.


## Modular Architecture

Version 29 separates the implementation into functional modules while still producing one `PoP_UniversalPatch.asi` file:

- `Core` — paths, Unicode INI handling, logging, PE utilities, unified hook management, and module lifecycle.
- `SettingsRegistry` — text-language and game registry-setting mapping.
- `Voice` — voice-pack detection, fallback, transactional patching, and SteamStub runtime handling.
- `Display`, `DPI`, `Performance`, and `Input` — isolated display, scaling, CPU, and input features.
- `TextureLoader` — TPF parsing, package priority, D3D9 hooks, hash matching, and replacement-texture lifetime.
- `Diagnostics` — unified compatibility reporting.

Text and voice initialization run early because they are required during game startup. Other modules initialize afterward. Main-executable IAT hooks are installed through one hook manager to prevent modules from overwriting each other.

## Safety Design

- The patch does not modify the game executable on disk.
- All changes are applied only in the game process and disappear when the game exits.
- All four voice patch points must match before they are written.
- Missing voice packs cause a safe fallback or disable voice patching.
- If required bytes do not match, the corresponding patch is skipped instead of being forced.
- On SteamStub-protected builds, fixed and verified runtime RVAs are checked only after the original game code has been unpacked in memory.

## Troubleshooting

### ASI Loader Reports Error 126

This usually means that the ASI has a missing hard dependency. Confirm that:

- The ASI was compiled as 32-bit.
- MSVC used the `/MT` static runtime.
- The project does not link `dinput8.lib`, `dxguid.lib`, or `xinput.lib`.

### Text Works, but There Is No Voice Audio

Check the following:

1. Confirm that the `DataPC_StreamedSoundsXXX.forge` file selected by `VoiceLanguage` exists.
2. Confirm that `[Voice] Enable`, `AssetPatch`, and `RequireAssetPatch` are all set to `1`.
3. Check `PoP_UniversalPatch.log` and `PoP_CompatibilityReport.txt` for the effective voice language and voice-patch status.

### Voice Automatically Falls Back to English

The requested voice pack is missing and the following settings are active:

```ini
AutoFallback=1
FallbackLanguage=1
```

The current session therefore uses English voice audio safely. The original `VoiceLanguage` value is not overwritten.

### The Windows Key Still Does Not Work

Confirm that:

- `WindowMode=1` or `WindowMode=2`
- `AllowWinKeyInWindowed=1`
- The log reports that the DirectInput hook was installed successfully

The patch does not restore the Windows key in fullscreen mode.

### The Mouse Is Still Confined After Alt+Tab

Confirm that:

- `WindowMode=1` or `WindowMode=2`
- `ReleaseMouseOnFocusLoss=1`

If the issue remains, test again with `LogLevel=2` or `3`.


### A TPF Package Loads, but No Texture Is Replaced

Check the following:

1. `[TextureLoader] Enable=1`.
2. The package is listed as `Package1`, `Package2`, and so on, and the path is valid relative to the ASI directory.
3. The log contains `Texture package database ready` and `Direct3D 9 texture hooks installed`.
4. The package uses classic TexMod hash definitions and contains a valid `texmod.def`.
5. Look for `Texture replaced: hash=...` messages in the log.

For diagnostics, temporarily use:

```ini
[Debug]
LogLevel=3
TextureLogUnmatched=1
TextureMaxUnmatchedLogs=256
```

Return `TextureLogUnmatched` to `0` for normal use to avoid a large unmatched-texture log.

### Configuration Values Were Rewritten

This is the configuration-validation feature. Invalid or out-of-range values are corrected and written back to `PoP_UniversalPatch.ini`. When `LogLevel` is at least `1`, the corrections are recorded in the log.

## Reporting Problems

Include the following files and information with a bug report:

- `PoP_UniversalPatch.ini`
- `PoP_UniversalPatch.log`, preferably generated with `LogLevel=2` or `3`
- `PoP_CompatibilityReport.txt`
- The game release and whether the executable has been unpacked or otherwise modified

## Building

The ASI must be compiled as 32-bit, with the C/C++ runtime and zlib linked statically. The texture module uses zlib to read TPF/ZIP data, but the released ASI should not require `zlib1.dll` or MinGW runtime DLLs.

MSVC:

```bat
cmake --preset msvc-x86-release
cmake --build --preset msvc-x86-release
```

For MSVC, install the static 32-bit zlib triplet with vcpkg:

```bat
vcpkg install zlib:x86-windows-static
```

Do not add hard links to the following libraries:

```text
dinput8.lib
dxguid.lib
xinput.lib
```

The DirectInput Windows-key fix uses dynamic function resolution and COM vtable hooking, so these hard dependencies are not required. D3DX9 is resolved dynamically as well. After building, use `dumpbin /dependents` or `objdump -p` to confirm that the ASI does not depend on `zlib1.dll`, `libwinpthread-1.dll`, `libstdc++-6.dll`, or `libgcc_s_*.dll`.

## Dependencies

Runtime:

- [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader), or another compatible 32-bit ASI loader
- An available D3DX9 DLL when the texture module is enabled, normally supplied by the Microsoft DirectX End-User Runtimes (June 2010)

Build-time:

- CMake
- A 32-bit MSVC or MinGW-w64 toolchain
- Static 32-bit zlib
