#!/bin/bash
# try.sh <mode> [frames] [skin.exe args]: makes the mod in that mode
# (make_mod.py), runs skin.exe in run and prints what the shim said. Mode 8
# runs with the draws checking vb0 and vb2 only, as importers that leave the
# index buffer and vb1 out do.
cd "$(dirname "$0")" || exit 1
[ -x run/skin.exe ] || { echo "no run folder: run setup.sh first"; exit 1; }
rm -rf run/Mods/skin run/ShaderCache/* run/d3d11_log.txt run/dx12_report.txt
python make_mod.py run/Mods/skin $1 || exit 1
if [ "$1" = 8 ]; then
	grep -v "CheckTextureOverride = ib\|CheckTextureOverride = vb1" ../dx12/case/Mods/harness.ini > run/Mods/harness.ini
else
	cp ../dx12/case/Mods/harness.ini run/Mods/harness.ini
fi
(cd run && ./skin.exe frames=${2:-60} dump=mode$1.bmp $3)
status=$?
cp ../dx12/case/Mods/harness.ini run/Mods/harness.ini
grep -a "\*\*\*\|error\|Skinning: [0-9]" run/dx12_report.txt | grep -v "warning 1328" | sed 's/Descriptor tables.*//' | sort | uniq -c | head -20
exit $status
