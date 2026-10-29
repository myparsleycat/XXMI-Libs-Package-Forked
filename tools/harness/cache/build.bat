@echo off
call "%~dp0..\common\msvc.bat" || exit /b 1
set "cache_source=%~dp0..\..\..\DirectX11\ResourceHash.cpp"
if not "%~1"=="" set "cache_source=%~f1"
cd /d "%~dp0"
if not exist bin mkdir bin
python make_impl.py "%cache_source%" bin\cache_impl.inc || exit /b 1
cl /nologo /O2 /EHsc /MT /std:c++14 /W3 /I..\..\.. /I..\..\..\crc32c-hw-1.0.5\include /I..\..\..\BinaryDecompiler /I..\..\..\BinaryDecompiler\include /DUNICODE /D_UNICODE /DMIGOTO_DX=11 /DCRC32C_STATIC=1 /Fe:bin\cache.exe /Fo:bin\ harness.cpp || exit /b 1
