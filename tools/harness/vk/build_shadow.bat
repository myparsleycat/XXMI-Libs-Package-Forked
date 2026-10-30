@echo off
call "%~dp0..\common\msvc.bat" || exit /b 1
set "shadow_source=%~dp0..\..\..\DirectX11\vk\VkDrawPath.cpp"
set "upload_source=%~dp0..\..\..\DirectX11\dx12\ShimObjects.cpp"
if not "%~1"=="" set "shadow_source=%~f1"
if not "%~2"=="" set "upload_source=%~f2"
cd /d "%~dp0"
if not exist bin mkdir bin
python make_shadow.py "%shadow_source%" "%upload_source%" bin\shadow_impl.inc || exit /b 1
cl /nologo /O2 /EHsc /MT /std:c++17 /W3 /Fe:bin\shadow.exe /Fo:bin\ shadow.cpp || exit /b 1
