#!/bin/bash
# The D3D12 side: the harness of ../dx12, run on the deployed
# configuration of an importer whose game is a D3D12 one (ZZMI, WWMI) with all
# of its mods. The first meshes of the harness are the parts of the mods of
# scenes/<importer>.scene: their index buffer (WWMI: positions) has the hash
# the sections look for, and they are drawn with the range a section matches.
#
#   dx12.sh setup [importer...]            run folders dx12_cur (x64/Release) and dx12_ref (ref/d3d11.dll)
#   dx12.sh bench [importer...]            ms a frame of recording, without the DLL, ref and cur
#   dx12.sh check [importer...]            the picture of ref against cur
#   dx12.sh run <importer> <cur|ref> [arguments of the harness, e.g. profile]
#
# check and bench copy the DLL of x64/Release into dx12_cur first.
#
# MODS=N takes the parts of the first N mods of the scene (default 12),
# FIRST=K starts at the Kth, PARTS=N is the most parts (default 300),
# MESHES=N the meshes of the harness (default 600). check exits with 1 when
# the pictures differ or a run fails.
here="$(cd "$(dirname "$0")" && pwd)"
repo="$here/../../.."
dx="$here/../dx12"
runs="${XH_RUNS:-$HOME/.xh}"

list() {
	# hash, first index and index count of every part with a known count, of the
	# mods chosen; a section that names no first index (-1) is drawn from 0.
	# awk stops at PARTS itself: a head would hide its failure.
	local scene="$here/scenes/$1.scene"
	[ -r "$scene" ] || { echo "$scene missing, run xh.sh setup $1" >&2; return 1; }
	awk -v first=${FIRST:-0} -v mods=${MODS:-12} -v parts=${PARTS:-300} '
		/^object/ { n++; next }
		$1 == "mesh" && n > first && n <= first + mods {
			for (i = 3; i <= NF; i++) { split($i, r, ":"); s = r[1] < 0 ? 0 : r[1]; if (r[2] > 2 && s + r[2] < 400000 && !seen[$2 " " s " " r[2]]++) { if (p++ >= parts) exit; print $2, s, r[2], n - first - 1 } }
		}' "$scene"
}

run() {
	local importer=$1 name=$2
	shift 2
	local dir="$runs/$importer/dx12_$name" slot=
	# Without parts the run draws no mod, and ref and cur would match for nothing
	list $importer > "$dir/scene_list.txt" || return 1
	[ -s "$dir/scene_list.txt" ] || { echo "no parts in the scene of $importer" >&2; return 1; }
	grep -q "^mesh_slot vb0" "$here/scenes/$importer.scene" && slot=slot=vb0
	# No .start: a folder of an older setup.py whose game had no settings file
	if [ -f "$dir/d3dx_user.ini.start" ]; then
		cp "$dir/d3dx_user.ini.start" "$dir/d3dx_user.ini"
	else
		rm -f "$dir/d3dx_user.ini"
	fi
	(cd "$dir" && ./harness.exe scene=scene_list.txt $slot meshes=${MESHES:-600} "$@" 2>&1)
}

use_built_dll() {
	# check and bench test the DLL just built: it goes into the
	# cur folder $1 first. run takes the folder as it is (ab.sh XH_DLL).
	[ -d "$1" ] || return 0
	cp "$repo/x64/Release/d3d11.dll" "$1/d3d11.dll" || exit 1
	rm -f "$1/d3d11.pdb"
	[ -f "$repo/x64/Release/d3d11.pdb" ] && cp "$repo/x64/Release/d3d11.pdb" "$1/d3d11.pdb"
}

importers() {
	[ $# -gt 0 ] && echo "$@" || echo "ZZMI WWMI"
}

cmd=$1
shift
case $cmd in
setup)
	for i in $(importers "$@"); do
		for name in cur ref; do
			dll="$(cygpath -m "$repo/x64/Release/d3d11.dll")"
			[ $name = ref ] && dll="$(cygpath -m "$here/ref/d3d11.dll")"
			# Without extended_rendering the mods are not drawn on D3D12, and
			# ref and cur would match for nothing
			python "$here/setup.py" $i --name dx12_$name --dll "$dll" --api none --set Rendering.extended_rendering=1 || exit 1
			dir="$runs/$i/dx12_$name"
			cp "$dx/run/harness.exe" "$dx/run/harness.pdb" "$dir/" || exit 1
			# The harness is the game, and the dump of hashes costs time
			sed -i 's/^target = .*/target = harness.exe/; s/^require_admin = .*/require_admin = false/; s/^dx12_dump_hashes = .*/dx12_dump_hashes = 0/' "$dir/d3dx.ini"
		done
	done
	;;
bench)
	for i in $(importers "$@"); do
		use_built_dll "$runs/$i/dx12_cur"
		echo "$i nodll: $(run $i cur frames=${BENCH_FRAMES:-2300} nodll | tail -1)"
		for name in ref cur; do
			echo "$i $name: $(run $i $name frames=${BENCH_FRAMES:-2300} | tail -1)"
		done
	done
	;;
check)
	ok=1
	for i in $(importers "$@"); do
		rm -f "$runs/$i/dx12_ref/check.bmp" "$runs/$i/dx12_cur/check.bmp"
		use_built_dll "$runs/$i/dx12_cur"
		a=$(run $i ref frames=${CHECK_FRAMES:-900} dump=check.bmp); sa=$?
		b=$(run $i cur frames=${CHECK_FRAMES:-900} dump=check.bmp); sb=$?
		a=$(echo "$a" | grep -v "draws a frame\|of that\|^scene")
		b=$(echo "$b" | grep -v "draws a frame\|of that\|^scene")
		if [ $sa = 0 ] && [ $sb = 0 ] && [ -s "$runs/$i/dx12_cur/check.bmp" ] && [ "$a" = "$b" ] && cmp -s "$runs/$i/dx12_ref/check.bmp" "$runs/$i/dx12_cur/check.bmp"; then
			echo "$i: same picture ($(echo "$a" | head -1))"
		else
			echo "$i: DIFFERENT (exit codes ref $sa, cur $sb)"; echo "  ref: $a"; echo "  cur: $b"; ok=0
		fi
	done
	[ $ok = 1 ] && echo "CHECK OK" || { echo "CHECK FAILED"; exit 1; }
	;;
run)
	run "$@"
	;;
*)
	sed -n '2,19p' "$0"
	exit 1
	;;
esac
