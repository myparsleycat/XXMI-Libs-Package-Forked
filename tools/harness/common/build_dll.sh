#!/bin/bash
# build_dll.sh [rebuild]: builds x64/Release/d3d11.dll. Pass "rebuild" after a
# change to a header many files include: an incremental build can keep stale
# objects. Never run two of these at once.
here="$(cd "$(dirname "$0")" && pwd)"
repo="$here/../../.."
vswhere="/c/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe"
msbuild=$("$vswhere" -latest -products '*' -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | head -1 | tr -d '\r')
[ -n "$msbuild" ] || { echo "MSBuild not found: install Visual Studio 2022 with the C++ tools"; exit 1; }
target=DirectX11
[ "$1" = rebuild ] && target=DirectX11:Rebuild
cd "$repo" || exit 1
"$msbuild" StereovisionHacks.sln -t:$target -p:Configuration=Release -p:Platform=x64 -nr:false -v:m > "$here/build_dll.log" 2>&1
status=$?
grep -a ": error\|: fatal" "$here/build_dll.log" | head -20
ls -l x64/Release/d3d11.dll
exit $status
