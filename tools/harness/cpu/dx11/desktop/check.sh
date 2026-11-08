#!/bin/bash
# check.sh [d3d11.dll]: the desktop channel. The codec against the shared
# vectors of testdata/desktop-channel, then channel.exe in a fresh run folder
# with [Desktop] channel = 1 (default DLL x64/Release/d3d11.dll). Exits with
# 2 when a check fails. Build with ../build.bat first.
here="$(cd "$(dirname "$0")" && pwd)"
cd "$here/.." || exit 1
[ -x bin/vectors.exe ] && [ -x bin/channel.exe ] || { echo "no bin/vectors.exe or bin/channel.exe: run build.bat first"; exit 1; }
status=0
python desktop/vectors.py ./bin/vectors.exe ../../../../testdata/desktop-channel/v1 || status=2

dir=bin/channel-run
rm -rf "$dir"
../../common/install_dll.sh "$dir" ${1:+"$1"} || exit 1
python ../../common/make_ini.py "$dir/d3dx.ini" Desktop.channel=1 || exit 1
./bin/channel.exe "$(cygpath -aw "$dir")"
result=$?
[ $result = 0 ] || status=$result
[ $result = 0 ] || echo "log: $dir/d3d11_log.txt"
exit $status
