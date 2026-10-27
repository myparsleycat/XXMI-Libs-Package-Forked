#!/bin/bash
# install_dll.sh <run folder> [d3d11.dll]: puts the DLL to test (default
# x64/Release/d3d11.dll), its PDB when there is one, and the DLLs the package
# ships next to it (Dependencies) into a run folder.
here="$(cd "$(dirname "$0")" && pwd)"
repo="$here/../../.."
dir=$1
dll=${2:-$repo/x64/Release/d3d11.dll}
[ -n "$dir" ] || { echo "install_dll.sh <run folder> [d3d11.dll]"; exit 1; }
[ -f "$dll" ] || { echo "no DLL at $dll: build it with common/build_dll.sh"; exit 1; }
mkdir -p "$dir" || exit 1
cp "$dll" "$dir/d3d11.dll" || exit 1
# What a stopped vk/check.sh set aside would otherwise replace it at its next start
rm -f "$dir/d3d11.dll.new"
rm -f "$dir/d3d11.pdb"
[ -f "${dll%.dll}.pdb" ] && cp "${dll%.dll}.pdb" "$dir/d3d11.pdb"
cp "$repo/Dependencies/d3dcompiler_47.dll" "$repo/Dependencies/dxcompiler.dll" "$repo/Dependencies/dxil.dll" \
	"$repo/Dependencies/SPIRV-Tools-shared.dll" "$dir/" || exit 1
