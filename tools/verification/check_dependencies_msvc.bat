@echo off
setlocal
if "%~1"=="" (
  echo Usage: %~nx0 path\to\PoP_UniversalPatch.asi
  exit /b 2
)
dumpbin /dependents "%~1"
endlocal
