@echo off
setlocal
set "REPO=%~dp0..\.."
pushd "%REPO%"
rem Requires Visual Studio and static x86 zlib from vcpkg.
rem   vcpkg install zlib:x86-windows-static
if "%VCPKG_ROOT%"=="" (
  echo VCPKG_ROOT is not set.
  popd
  exit /b 1
)
cmake -S . -B build\msvc-x86 -A Win32 ^
  -DCMAKE_TOOLCHAIN_FILE="%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake" ^
  -DVCPKG_TARGET_TRIPLET=x86-windows-static
if errorlevel 1 (
  popd
  exit /b 1
)
cmake --build build\msvc-x86 --config Release
set ERR=%ERRORLEVEL%
popd
exit /b %ERR%
