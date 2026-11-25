@echo off
call "%~dp0..\common\msvc.bat" || exit /b 1
set "bindings_draw=%~dp0..\..\..\DirectX11\vk\VkDrawPath.cpp"
set "bindings_context=%~dp0..\..\..\DirectX11\dx12\ShimContext.cpp"
if not "%~1"=="" set "bindings_draw=%~f1"
if not "%~2"=="" set "bindings_context=%~f2"
cd /d "%~dp0"
if not exist bin mkdir bin
python make_bindings.py "%bindings_draw%" "%bindings_context%" bin\bindings_types.inc bin\bindings_impl.inc || exit /b 1
cl /nologo /std:c++23 /Zc:__cplusplus /O2 /EHsc /MT /W3 /I..\..\.. /I..\..\..\Vulkan-Headers\include /Fe:bin\bindings.exe /Fo:bin\ bindings.cpp || exit /b 1
