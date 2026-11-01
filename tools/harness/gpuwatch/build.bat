@echo off
call "%~dp0..\common\msvc.bat" || exit /b 1
cd /d "%~dp0"
if not exist bin mkdir bin
cl /nologo /O2 /EHsc /W3 /D_CRT_SECURE_NO_WARNINGS /Fe:bin\hang.exe /Fo:bin\ hang.cpp user32.lib d3dcompiler.lib
cl /nologo /O2 /EHsc /W3 /D_CRT_SECURE_NO_WARNINGS /Fe:bin\decode.exe /Fo:bin\ decode.cpp
