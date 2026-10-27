#!/bin/bash
# setup.sh [folder] [d3d11.dll] [Section.key=value...]: makes a run folder
# (default run) with skin.exe and its shaders of bin (build.bat), the DLL to
# test (default x64/Release/d3d11.dll), the d3dx.ini of the package with the
# D3D12 debug layer, the hash dump and the log on, and the per-draw work of an
# importer (../dx12/case/Mods/harness.ini). try.sh and check.sh add the mod.
here="$(cd "$(dirname "$0")" && pwd)"
cd "$here" || exit 1
dir=${1:-run}
dll=$2
shift $(( $# < 2 ? $# : 2 ))
[ -f bin/skin.exe ] || { echo "no bin/skin.exe: run build.bat first"; exit 1; }
rm -rf "$dir/Mods" "$dir/ShaderCache"
bash ../common/install_dll.sh "$dir" "$dll" || exit 1
cp bin/skin.exe bin/skin.pdb bin/skin_cs.cso bin/draw_vs.cso bin/draw_ps.cso "$dir/" || exit 1
python ../common/make_ini.py "$dir/d3dx.ini" System.check_foreground_window=0 Hunting.dx12_dump_hashes=1 \
	Rendering.dx12_debug_layer=1 Logging.log_level=info Logging.unbuffered=1 "$@" || exit 1
mkdir -p "$dir/Mods" && cp ../dx12/case/Mods/harness.ini "$dir/Mods/" || exit 1
echo "$dir ready"
