@echo off
setlocal
if "%~1"=="" (
  echo Usage: %~nx0 path\to\PoP_UniversalPatch.asi
  exit /b 2
)
objdump -p "%~1" | findstr "DLL Name"
endlocal
