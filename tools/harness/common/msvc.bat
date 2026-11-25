@echo off
if /i "%VSCMD_ARG_TGT_ARCH%"=="x64" call :toolset_ok && exit /b 0

set "vswhere=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "vsdir="
rem "call" keeps cmd from stripping the first and last quotes of a command that starts with one
if exist "%vswhere%" for /f "usebackq delims=" %%i in (`call "%vswhere%" -latest -products * -version "[18.12,19.0)" -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "vsdir=%%i"
if not defined vsdir goto :missing_toolset

rem vcvars64 reports a missing vswhere.exe on PATH on stderr and its own errors on stdout
call "%vsdir%\VC\Auxiliary\Build\vcvars64.bat" 10.0.26100.0 >nul 2>&1 || (
	echo vcvars64.bat failed in "%vsdir%". The x64 C++ tools and Windows SDK 10.0.26100.0 are required. 1>&2
	exit /b 1
)
call :toolset_ok && exit /b 0

:missing_toolset
echo C++23 requires Visual Studio 2026 18.12 or later with MSVC Build Tools 14.52 or later. 1>&2
exit /b 1

:toolset_ok
if not defined VCToolsVersion exit /b 1
for /f "tokens=1,2 delims=." %%a in ("%VCToolsVersion%") do (
	if %%a GTR 14 exit /b 0
	if %%a EQU 14 if %%b GEQ 52 exit /b 0
)
exit /b 1
