@echo off
call "%~dp0..\common\msvc.bat" || exit /b 1
cd /d "%~dp0"
if not exist bin mkdir bin
cl /nologo /std:c++23 /Zc:__cplusplus /O2 /EHsc /W3 /Zi /D_CRT_SECURE_NO_WARNINGS /Fe:bin\harness.exe /Fo:bin\ /Fd:bin\harness.pdb harness.cpp
