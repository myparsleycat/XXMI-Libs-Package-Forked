@echo off
call "%~dp0..\common\msvc.bat" || exit /b 1
cd /d "%~dp0"
if not exist bin mkdir bin
set "dxc=%WindowsSdkVerBinPath%x64\dxc.exe"
if not exist "%dxc%" goto no_dxc
"%dxc%" -nologo -T cs_6_0 -E main -Fo bin\skin_cs.cso skin_cs.hlsl || exit /b 1
"%dxc%" -nologo -T cs_6_0 -E main -D RIGID -Fo bin\skin_rigid_cs.cso skin_cs.hlsl || exit /b 1
"%dxc%" -nologo -T vs_6_0 -E vs -Fo bin\draw_vs.cso draw.hlsl || exit /b 1
"%dxc%" -nologo -T vs_6_0 -E vs -D PLACE -Fo bin\draw_vs_place.cso draw.hlsl || exit /b 1
"%dxc%" -nologo -T ps_6_0 -E ps -Fo bin\draw_ps.cso draw.hlsl || exit /b 1
"%dxc%" -nologo -T vs_6_0 -E vs -D BINDINGS -Fo bin\draw_vs_bindings.cso draw.hlsl || exit /b 1
"%dxc%" -nologo -T ps_6_0 -E ps -D BINDINGS -Fo bin\draw_ps_bindings.cso draw.hlsl || exit /b 1
cl /nologo /O2 /EHsc /W3 /Zi /D_CRT_SECURE_NO_WARNINGS /Fe:bin\skin.exe /Fo:bin\ /Fd:bin\skin.pdb skin.cpp
exit /b %errorlevel%

:no_dxc
echo dxc.exe not found in the Windows SDK bin folder
exit /b 1
