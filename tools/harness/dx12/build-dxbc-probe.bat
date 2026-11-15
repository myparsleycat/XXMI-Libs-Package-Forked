@echo off
call "%~dp0..\common\msvc.bat" || exit /b 1
cd /d "%~dp0"
if not exist bin mkdir bin
cl /nologo /O2 /EHsc /W3 /D_CRT_SECURE_NO_WARNINGS /Fe:bin\probe_assemble.exe /Fo:bin\ probe_assemble.cpp ..\..\..\D3D_Shaders\Assembler.cpp d3dcompiler.lib
