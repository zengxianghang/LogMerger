@echo off
setlocal EnableExtensions

set "SCRIPT_DIR=%~dp0"
for %%I in ("%SCRIPT_DIR%..") do set "ROOT_DIR=%%~fI"
set "SOURCE=%ROOT_DIR%\merge_aux_into_input.cpp"
set "OUTPUT=%ROOT_DIR%\merge_aux_into_input.exe"

if not exist "%SOURCE%" (
  echo [ERROR] Source file not found: "%SOURCE%"
  exit /b 1
)

where cl.exe >nul 2>nul
if not errorlevel 1 (
  echo [INFO] Using cl.exe already available in PATH.
  goto :build
)

set "VCVARS="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"

rem Prefer vswhere because it also handles non-default Visual Studio install paths.
if exist "%VSWHERE%" (
  for /f "usebackq tokens=*" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
    if exist "%%I\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%%I\VC\Auxiliary\Build\vcvars64.bat"
  )
)

if defined VCVARS goto :setup_vc

rem Fallback for machines where vswhere is missing but Visual Studio is installed
rem in one of the standard locations.
for %%V in (2022 2019 2017) do (
  for %%E in (Community Professional Enterprise BuildTools) do (
    if exist "%ProgramFiles%\Microsoft Visual Studio\%%V\%%E\VC\Auxiliary\Build\vcvars64.bat" (
      set "VCVARS=%ProgramFiles%\Microsoft Visual Studio\%%V\%%E\VC\Auxiliary\Build\vcvars64.bat"
      goto :setup_vc
    )
    if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\%%V\%%E\VC\Auxiliary\Build\vcvars64.bat" (
      set "VCVARS=%ProgramFiles(x86)%\Microsoft Visual Studio\%%V\%%E\VC\Auxiliary\Build\vcvars64.bat"
      goto :setup_vc
    )
  )
)

goto :vs_not_found

:setup_vc
echo [INFO] Initializing Visual Studio x64 build environment:
echo        %VCVARS%
call "%VCVARS%" >nul
if errorlevel 1 (
  echo [ERROR] Failed to initialize the Visual Studio build environment.
  exit /b 1
)

where cl.exe >nul 2>nul
if errorlevel 1 (
  echo [ERROR] Visual Studio was found, but cl.exe is still unavailable.
  echo         Make sure the Desktop development with C++ workload is installed.
  exit /b 1
)

goto :build

:vs_not_found
echo [ERROR] Visual Studio C++ build tools were not found.
echo         Install Visual Studio with the Desktop development with C++ workload.
echo         The script searches via vswhere first, then standard VS 2022/2019/2017 paths.
exit /b 1

:build
echo [INFO] Building LogMerger with MSVC...
pushd "%ROOT_DIR%" >nul
cl /nologo /O2 /std:c++17 /EHsc /Fe:"%OUTPUT%" "%SOURCE%"
set "BUILD_RC=%ERRORLEVEL%"
popd >nul

if not "%BUILD_RC%"=="0" (
  echo [ERROR] Build failed with exit code %BUILD_RC%.
  exit /b %BUILD_RC%
)

echo [OK] Build succeeded: "%OUTPUT%"
endlocal
exit /b 0
