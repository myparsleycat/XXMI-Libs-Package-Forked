"""Build an opt-in, test-only DLL and WARP fixture into an external directory."""
import argparse, subprocess as sp
from pathlib import Path
from xml.sax.saxutils import escape

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("output",type=Path)
    p.add_argument("--vs",type=Path,default=Path(r"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"))
    p.add_argument("--config",default="Release",choices=("Release","Zip Release"))
    a=p.parse_args()
    root=Path(__file__).resolve().parents[3]
    out=a.output.resolve()
    if out==root or root in out.parents: p.error("Use an empty directory outside the source tree")
    out.mkdir(parents=True,exist_ok=False)
    if not (a.vs/"MSBuild/Current/Bin/MSBuild.exe").is_file():p.error("Pass --vs for your Visual Studio installation")
    (out/"hook.cpp").write_text(
        '#include "'+(root/"DirectX11/Hunting.cpp").as_posix()+'"\n'
        'extern "C" __declspec(dllexport) void FrameAnalysisTestRequest() {\n'
        ' Dx12ShimLock(); if(dx12_shim) AnalyseFrame(dx12_shim->hacker_device,nullptr); Dx12ShimUnlock();\n}\n'
        'extern "C" __declspec(dllexport) BOOL FrameAnalysisTestState() { return G->analyse_frame; }\n',
        encoding="utf-8-sig")
    props=out/"test.props"
    props.write_text(
        '<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003">\n'
        ' <PropertyGroup><OutDir>'+escape(str(out/"dll"))+'\\</OutDir>'
        '<IntDir>'+escape(str(out/"obj"/"$(MSBuildProjectName)"))+'\\</IntDir></PropertyGroup>\n'
        ' <ItemGroup Condition="\'$(MSBuildProjectName)\' == \'DirectX11\'">'
        '<ClCompile Remove="Hunting.cpp" /><ClCompile Include="'+escape(str(out/"hook.cpp"))+'" />'
        '</ItemGroup>\n</Project>',encoding="utf-8")
    args=[a.vs/"MSBuild/Current/Bin/MSBuild.exe",root/"StereovisionHacks.sln",
        "/t:DirectX11","/nologo","/m:4","/v:minimal","/nr:false",
        "/p:Configuration="+a.config,"/p:Platform=x64","/p:PlatformToolset=v143",
        "/p:WindowsTargetPlatformVersion=10.0",
        "/p:ForceImportBeforeCppTargets="+str(props),
        "/p:PostBuildEventUseInBuild=false","/p:PreBuildEventUseInBuild=false","/p:PreLinkEventUseInBuild=false"]
    with (out/"dll_build.log").open("wb") as log:
        result=sp.run(list(map(str,args)),cwd=root,stdout=log,stderr=sp.STDOUT)
    if result.returncode: raise SystemExit(f"Build failed; see {out/'dll_build.log'}")
    cmd=out/"build_harness.cmd"
    cmd.write_text('@echo off\nchcp 65001 >nul\ncall "'+str(a.vs/"VC/Auxiliary/Build/vcvars64.bat")+
        '" >nul\ncd /d "'+str(out)+'"\ncl /nologo /EHsc /DNOMINMAX /std:c++17 "'+
        str(Path(__file__).with_name("harness.cpp"))+
        '" /Fe:harness.exe d3d11.lib d3d12.lib dxgi.lib d3dcompiler.lib user32.lib\nexit /b %errorlevel%\n',
        encoding="utf-8")
    with (out/"harness_build.log").open("wb") as log:
        sp.run(["cmd","/d","/c",cmd],cwd=out,stdout=log,stderr=sp.STDOUT,check=True)
    dll=out/"dll/d3d11.dll"
    if not dll.is_file():raise SystemExit("Test DLL was not produced")
    print(dll)
    print("Test-only DLL: do not install it in a game.")
if __name__=="__main__":main()
