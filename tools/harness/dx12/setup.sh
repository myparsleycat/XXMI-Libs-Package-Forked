#!/bin/bash
# setup.sh [folder] [d3d11.dll] [Section.key=value...]: makes a run folder
# (default run) with the harness of bin (build.bat), the DLL to test (default
# x64/Release/d3d11.dll), the d3dx.ini of the package with the settings the
# harness needs and the ones given, and the mods of case/ plus 300
# TextureOverride sections that match nothing.
#   setup.sh                      run with the DLL just built
#   setup.sh ref old/d3d11.dll    ref, the folder check.sh ref compares run with
here="$(cd "$(dirname "$0")" && pwd)"
cd "$here" || exit 1
dir=${1:-run}
dll=$2
shift $(( $# < 2 ? $# : 2 ))
[ -f bin/harness.exe ] || { echo "no bin/harness.exe: run build.bat first"; exit 1; }
rm -rf "$dir/Mods" "$dir/ShaderCache"
bash ../common/install_dll.sh "$dir" "$dll" || exit 1
cp bin/harness.exe bin/harness.pdb "$dir/" || exit 1
python ../common/make_ini.py "$dir/d3dx.ini" System.check_foreground_window=0 "$@" || exit 1
cp -r case/Mods "$dir/" || exit 1
python make_overrides.py "$dir/Mods/overrides.ini" || exit 1
echo "$dir ready"
