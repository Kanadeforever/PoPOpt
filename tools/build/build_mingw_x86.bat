@echo off
setlocal
set "REPO=%~dp0..\.."
pushd "%REPO%"
rem Requires i686-w64-mingw32-g++ and a matching static 32-bit libz.a.
if "%ZLIB_STATIC_LIB%"=="" (
  echo ZLIB_STATIC_LIB is not set to a 32-bit static libz.a.
  popd
  exit /b 1
)
i686-w64-mingw32-g++ -std=c++17 -O2 -shared -static ^
  -static-libgcc -static-libstdc++ ^
  -DWIN32_LEAN_AND_MEAN -DNOMINMAX -Isrc ^
  src\Main.cpp ^
  src\core\Core.cpp src\core\PeUtils.cpp src\core\LanguageIds.cpp ^
  src\modules\SettingsRegistryModule.cpp src\modules\VoiceModule.cpp ^
  src\modules\DisplayModule.cpp src\modules\DpiModule.cpp ^
  src\modules\CpuModule.cpp src\modules\InputModule.cpp ^
  src\modules\GraphicsModule.cpp src\modules\TextureLoaderModule.cpp ^
  src\modules\DiagnosticsModule.cpp src\texture\TpfArchive.cpp ^
  "%ZLIB_STATIC_LIB%" -luser32 -ladvapi32 ^
  -o PoP_UniversalPatch.asi
if errorlevel 1 (
  popd
  exit /b 1
)
objdump -p PoP_UniversalPatch.asi | findstr "DLL Name"
set ERR=%ERRORLEVEL%
popd
exit /b %ERR%
