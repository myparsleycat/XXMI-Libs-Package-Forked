"""Run the hidden WARP capture-window tests; neither sends keys nor starts a game."""
import argparse, hashlib, json, os, re, shutil, struct, subprocess as sp
from pathlib import Path

def run(build, runtime, out, gap, action="", textures=True, expected="success"):
    out.mkdir()
    exe=build/"harness.exe"
    shutil.copy2(build/"dll/d3d11.dll",out/"d3d11.dll")
    for name in ("d3dcompiler_47.dll","dxcompiler.dll","dxil.dll"):
        source=runtime/name
        if not source.is_file():raise ValueError("Missing runtime dependency: "+str(source))
        shutil.copy2(source,out/name)
    text="""[Loader]
target = harness.exe
require_admin = false
[Logging]
log_level = info
unbuffered = 1
[System]
skip_early_includes_load = false
check_foreground_window = 0
[Rendering]
extended_rendering = true
shader_hash = 3dmigoto
[Hunting]
hunting = 1
analyse_frame = VK_F8
analyse_options = """+("dump_tex" if textures else "txt")+"\n"
    (out/"d3dx.ini").write_text(text,encoding="utf-8")
    sp.run([exe,"compile",out],cwd=out,check=True,timeout=20)
    env={**os.environ,"CAPTURE_GAP_PRESENTS":str(gap),"CAPTURE_ACTION":action}
    p=sp.run([exe,"dx12",out/"d3d11.dll",out],cwd=out,env=env,capture_output=True,timeout=90)
    (out/"stdout.log").write_bytes(p.stdout);(out/"stderr.log").write_bytes(p.stderr)
    assert p.returncode==0,(out.name,p.returncode,p.stderr.decode(errors="replace"))
    assert json.loads(p.stdout)==dict(red=256,green=0,black=0,other=0),"Capture changed the render result"
    states=re.findall(r"capture_state\[\d+\]=(\d+)",p.stderr.decode("utf-8",errors="replace"))
    assert list(map(int,states))==[int(n<5) for n in range(1,gap+1)],states
    caps=sorted(p for p in out.glob("FrameAnalysis-*") if p.is_dir())
    assert len(caps)==(2 if action=="retry" else 1),(out.name,caps)
    target=caps[-1]
    files=sorted(str(p.relative_to(target)) for p in target.rglob("*") if p.is_file())
    result=dict(case=out.name,gap=gap,action=action,files=files,exit_code=p.returncode)
    (out/"evidence.json").write_text(json.dumps(result,indent=2),encoding="utf-8")
    if expected=="success":
        assert (target/"log.txt").is_file(),"No log after an eligible draw: "+str(target)
        log=(target/"log.txt").read_text("utf-8")
        assert "DrawIndexedInstanced" in log
        dds=list(target.rglob("*.dds"))
        assert len(dds)==(2 if textures else 0),(out.name,files)
        hashes=[]
        for path in dds:
            data=path.read_bytes()
            assert data[:4]==b"DDS " and struct.unpack_from("<II",data,12)==(16,16)
            assert data[148:]==bytes((255,0,0,255))*256
            hashes.append(hashlib.sha256(data).hexdigest())
        result["dds_sha256"]=hashes
        assert not (target/"capture_status.txt").exists()
    elif expected=="timeout":
        assert files==["capture_status.txt"],files
        assert "No DX12 draw" in (target/"capture_status.txt").read_text("utf-8")
    elif expected=="abort":
        assert files==[],files
    if action=="retry":
        assert sorted(x.name for x in caps[0].iterdir())==["capture_status.txt"]
    # Continue recording and rendering after a disarmed request must not
    # create a DDS or append a log to the timed-out/aborted directory.
    (out/"evidence.json").write_text(json.dumps(result,indent=2),encoding="utf-8")
    print(out.name+": PASS",flush=True)
    return result

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument("build",type=Path,help="build.py output (test-only DLL)")
    ap.add_argument("runtime",type=Path,help="folder with DXC/DXIL and d3dcompiler_47 DLLs")
    ap.add_argument("output",type=Path,help="new external test result directory")
    ap.add_argument("--only-gap",type=int,choices=range(7))
    a=ap.parse_args()
    build=a.build.resolve();runtime=a.runtime.resolve();out=a.output.resolve()
    out.mkdir(parents=True,exist_ok=False)
    tests=[(f"gap_{n}",n,"",True,"success" if n<=4 else "timeout") for n in range(7)]
    tests += [("abort_pending",2,"abort",True,"abort"),
              ("retry_after_timeout",5,"retry",True,"success"),
              ("log_only",1,"",False,"success")]
    if a.only_gap is not None:tests=[tests[a.only_gap]]
    result=[run(build,runtime,out/name,n,action,tex,expected) for name,n,action,tex,expected in tests]
    (out/"results.json").write_text(json.dumps(result,indent=2),encoding="utf-8")
if __name__=="__main__":main()
