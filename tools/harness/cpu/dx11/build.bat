@echo off
call "%~dp0..\..\common\msvc.bat" || exit /b 1
cd /d "%~dp0"
if not exist bin mkdir bin
cl /nologo /O2 /EHsc /W3 /Zi /D_CRT_SECURE_NO_WARNINGS /Fe:bin\harness.exe /Fo:bin\ /Fd:bin\harness.pdb harness.cpp trace.cpp user32.lib dbghelp.lib || exit /b 1
cl /nologo /O2 /EHsc /W3 /Zi /std:c++17 /utf-8 /D_CRT_SECURE_NO_WARNINGS /Fe:bin\vectors.exe /Fo:bin\ /Fd:bin\vectors.pdb desktop\vectors.cpp ..\..\..\..\DirectX11\desktop\Protocol.cpp || exit /b 1
cl /nologo /O2 /EHsc /W3 /Zi /std:c++17 /utf-8 /D_CRT_SECURE_NO_WARNINGS /Fe:bin\pick.exe /Fo:bin\ /Fd:bin\pick.pdb desktop\pick.cpp ..\..\..\..\DirectX11\desktop\Protocol.cpp user32.lib d3dcompiler.lib || exit /b 1
cl /nologo /O2 /EHsc /W3 /Zi /std:c++17 /utf-8 /D_CRT_SECURE_NO_WARNINGS /Fe:bin\client.exe /Fo:bin\ /Fd:bin\client.pdb desktop\client.cpp ..\..\..\..\DirectX11\desktop\Protocol.cpp advapi32.lib
