@echo off
call "%~dp0..\..\common\msvc.bat" || exit /b 1
cd /d "%~dp0"
if not exist bin mkdir bin
cl /nologo /O2 /EHsc /W3 /Zi /D_CRT_SECURE_NO_WARNINGS /Fe:bin\harness.exe /Fo:bin\ /Fd:bin\harness.pdb harness.cpp trace.cpp user32.lib dbghelp.lib
