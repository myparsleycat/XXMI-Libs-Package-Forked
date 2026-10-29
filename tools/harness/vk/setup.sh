#!/bin/bash
# setup.sh [folder] [d3d11.dll] [Section.key=value...]: makes a run folder
# (default run) with the harness and shaders of bin (build.bat), the DLL to
# test (default x64/Release/d3d11.dll), the d3dx.ini of the package with the
# settings the harness needs and the ones given, and the files the test inis
# of case/tests read. Mods/test.ini starts as test_texture.
#   setup.sh                      run with the DLL just built
#   setup.sh ref old/d3d11.dll    ref, the folder check.sh compares run with
here="$(cd "$(dirname "$0")" && pwd)"
cd "$here" || exit 1
dir=${1:-run}
dll=$2
shift $(( $# < 2 ? $# : 2 ))
[ -f bin/harness.exe ] || { echo "no bin/harness.exe: run build.bat first"; exit 1; }
rm -rf "$dir/Mods" "$dir/ShaderCache"
bash ../common/install_dll.sh "$dir" "$dll" || exit 1
cp bin/harness.exe bin/harness.pdb bin/vs.spv bin/ps.spv bin/efmi_vs.spv bin/efmi_ps.spv "$dir/" || exit 1
python ../common/make_ini.py "$dir/d3dx.ini" Loader.target=harness.exe Loader.require_admin=false \
	System.check_foreground_window=0 Rendering.track_region_hashes=1 Hunting.vk_dump_hashes=1 \
	Logging.log_level=info Logging.unbuffered=1 "$@" || exit 1
cp -r case/Mods "$dir/" || exit 1
python make_dds.py "$dir/Mods/tex_bc7.dds" || exit 1
cp case/tests/test_texture.ini "$dir/Mods/test.ini" || exit 1
echo "$dir ready"
