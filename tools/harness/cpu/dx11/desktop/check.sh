#!/bin/bash
# check.sh [d3d11.dll]: the desktop channel. The codec against the shared
# vectors of testdata/desktop-channel, then pick.exe in a fresh run folder
# with [Desktop] channel = 1 (default DLL x64/Release/d3d11.dll). Exits with
# 2 when a check fails. Build with ../build.bat first.
here="$(cd "$(dirname "$0")" && pwd)"
cd "$here/.." || exit 1
[ -x bin/vectors.exe ] && [ -x bin/pick.exe ] || { echo "no bin/vectors.exe or bin/pick.exe: run build.bat first"; exit 1; }
status=0
python desktop/vectors.py ./bin/vectors.exe ../../../../testdata/desktop-channel/v1 || status=2

for mode in basic uav region devices cbzero indirect shader-uav limits coplanar stencil reconnect multiswap predication backpressure; do
    dir=bin/pick-run
    [ "$mode" = basic ] || dir=bin/pick-$mode-run
    rm -rf "$dir"
    ../../common/install_dll.sh "$dir" ${1:+"$1"} || exit 1
    settings=(Desktop.channel=1 "Desktop.inspect_key=no_modifiers VK_F23")
    [ "$mode" != region ] || settings+=(Rendering.track_region_hashes=1)
    python ../../common/make_ini.py "$dir/d3dx.ini" "${settings[@]}" || exit 1
    ./bin/pick.exe "$(cygpath -aw "$dir")" "$mode"
    result=$?
    [ $result = 0 ] || status=$result
    [ $result = 0 ] || echo "log: $dir/d3d11_log.txt"
done
exit $status
