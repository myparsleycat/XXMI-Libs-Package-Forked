@echo off
rem Sets up the x64 MSVC environment of the newest Visual Studio with the C++ tools, any edition.
rem An x86 developer prompt sets VCToolsInstallDir as well
if defined VCToolsInstallDir if /i "%VSCMD_ARG_TGT_ARCH%"=="x64" exit /b 0
set "vswhere=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%vswhere%" (
	echo vswhere.exe not found: install Visual Studio 2022 with the C++ tools
	exit /b 1
)
set "vsdir="
for /f "usebackq delims=" %%i in (`"%vswhere%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "vsdir=%%i"
if not defined vsdir (
	echo no Visual Studio with the C++ tools found
	exit /b 1
)
rem vcvars64 looks for vswhere.exe on PATH as well and says so when it is not there
call "%vsdir%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 || exit /b 1
