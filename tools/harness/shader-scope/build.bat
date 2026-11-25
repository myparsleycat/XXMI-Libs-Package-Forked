@echo off
setlocal
call "%~dp0..\common\msvc.bat" || exit /b 1
set "output=%~1"
if not defined output set "output=%~dp0bin"
if not exist "%output%" mkdir "%output%"
cl /nologo /std:c++23 /Zc:__cplusplus /O2 /EHsc /W4 /DNOMINMAX /I"%~dp0..\..\.." /Fe:"%output%\harness.exe" /Fo:"%output%\harness.obj" "%~dp0harness.cpp" d3d11.lib d3d12.lib dxgi.lib d3dcompiler.lib user32.lib dbghelp.lib
exit /b %errorlevel%
