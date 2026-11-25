@echo off
call "%~dp0..\common\msvc.bat" || exit /b 1
cd /d "%~dp0"
if not exist bin mkdir bin
cl /nologo /std:c++23 /Zc:__cplusplus /O2 /EHsc /W3 /Fe:bin\harness.exe /Fo:bin\ harness.cpp
