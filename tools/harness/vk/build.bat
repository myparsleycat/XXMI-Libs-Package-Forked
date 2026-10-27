@echo off
call "%~dp0..\common\msvc.bat" || exit /b 1
cd /d "%~dp0"
if not exist bin mkdir bin
if not defined VULKAN_SDK (
	echo VULKAN_SDK is not set: install the Vulkan SDK for glslangValidator
	exit /b 1
)
set "glslang=%VULKAN_SDK%\Bin\glslangValidator.exe"
"%glslang%" -V -S vert -o bin\vs.spv shader.vert >nul || exit /b 1
"%glslang%" -V -S frag -o bin\ps.spv shader.frag >nul || exit /b 1
"%glslang%" -V -S vert -o bin\efmi_vs.spv efmi.vert >nul || exit /b 1
"%glslang%" -V -S frag -o bin\efmi_ps.spv efmi.frag >nul || exit /b 1
cl /nologo /O2 /EHsc /W3 /Zi /I..\..\..\Vulkan-Headers\include /Fe:bin\harness.exe /Fo:bin\ /Fd:bin\harness.pdb harness.cpp user32.lib
