from pathlib import Path
import sys

root = Path(__file__).resolve().parents[2]
required = [
    "CMakeLists.txt", "README.md", "README.zh-CN.md",
    "src/Main.cpp", "src/core/Core.cpp",
    "src/modules/SettingsRegistryModule.cpp",
    "src/modules/VoiceModule.cpp",
    "src/modules/TextureLoaderModule.cpp",
    "config/PoP_UniversalPatch.ini",
]
missing = [p for p in required if not (root / p).exists()]
if missing:
    print("Missing required files:")
    for p in missing: print("  ", p)
    sys.exit(1)

banned_suffixes = {".asi", ".dll", ".exe", ".pdb", ".obj", ".lib"}
banned = [p for p in root.rglob("*") if p.is_file() and p.suffix.lower() in banned_suffixes]
if banned:
    print("Unexpected compiled/proprietary files:")
    for p in banned: print("  ", p.relative_to(root))
    sys.exit(1)

source_text = (root / "src/Main.cpp").read_text(encoding="utf-8")
for marker in ["InitializeEarlyLanguageVoiceFoundation", "RegisterEarlyHooks", "ApplyEarlyPatches"]:
    if marker not in source_text:
        print(f"Missing early-language/voice marker: {marker}")
        sys.exit(1)

print("Repository layout and source guards: OK")
