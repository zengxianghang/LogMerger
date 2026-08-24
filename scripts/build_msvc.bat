@echo off
setlocal

where cl.exe >nul 2>nul
if errorlevel 1 (
  echo Please run from Visual Studio x64 Native Tools Command Prompt.
  exit /b 1
)

cl /nologo /O2 /std:c++17 /EHsc /Fe:merge_aux_into_input.exe merge_aux_into_input.cpp

endlocal
