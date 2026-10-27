@echo off
call "%~dp0..\common\msvc.bat" || exit /b 1
cd /d "%~dp0"
if not exist bin mkdir bin
cl /nologo /O2 /EHsc /W3 /Fe:bin\harness.exe /Fo:bin\ harness.cpp
