#!/bin/bash
# The Vulkan side: the harness of ../vk, run on the deployed EFMI
# configuration with all of its mods. The parts drawn are those of the mods of
# scenes/EFMI.scene, their index data given the hashes the sections look for.
#
#   vk.sh setup                 run folders vk_cur (x64/Release) and vk_ref (ref/d3d11.dll)
#   vk.sh bench [arguments]     ms a frame of the mods' work, ref and cur
#   vk.sh check                 the picture and the draw counts of ref against cur
#   vk.sh run <cur|ref> [arguments of the harness, e.g. --profile]
#
# MODS=N takes the parts of the first N mods of the scene (default 6),
# FIRST=K starts at the Kth, PARTS=N is the most parts (default 400),
# SECONDS_RUN how long a run is (default 8); a part is drawn --characters x
# --passes times. check exits with 1 when the pictures differ or are missing.
# check and bench copy the DLL of x64/Release into vk_cur first.
here="$(cd "$(dirname "$0")" && pwd)"
repo="$here/../../.."
vk="$here/../vk"
root="${XH_RUNS:-$HOME/.xh}/EFMI"

list() {
	# hash and index count of every part with a known count, of the mods chosen;
	# awk stops at PARTS itself: a head would hide its failure
	local scene="$here/scenes/EFMI.scene"
	[ -r "$scene" ] || { echo "$scene missing, run xh.sh setup EFMI" >&2; return 1; }
	awk -v first=${FIRST:-0} -v mods=${MODS:-6} -v parts=${PARTS:-400} '
		/^object/ { n++; next }
		$1 == "mesh" && n > first && n <= first + mods {
			for (i = 3; i <= NF; i++) { split($i, r, ":"); if (r[2] > 2 && r[2] < 200000 && !seen[$2 " " r[2]]++) { if (p++ >= parts) exit; print $2, r[2], n - first - 1 } }
		}' "$scene"
}

use_built_dll() {
	# check and bench test the DLL just built: it goes into the
	# cur folder $1 first. run takes the folder as it is (ab.sh XH_DLL).
	[ -d "$1" ] || return 0
	cp "$repo/x64/Release/d3d11.dll" "$1/d3d11.dll" || exit 1
	rm -f "$1/d3d11.pdb"
	[ -f "$repo/x64/Release/d3d11.pdb" ] && cp "$repo/x64/Release/d3d11.pdb" "$1/d3d11.pdb"
}

run() {
	local name=$1
	shift
	local dir="$root/vk_$name"
	# No .start: a folder of an older setup.py whose game had no settings file
	if [ -f "$dir/d3dx_user.ini.start" ]; then
		cp "$dir/d3dx_user.ini.start" "$dir/d3dx_user.ini"
	else
		rm -f "$dir/d3dx_user.ini"
	fi
	# Without parts the run draws no mod, and ref and cur would match for nothing
	list > "$dir/efmi_list.txt" || return 1
	[ -s "$dir/efmi_list.txt" ] || { echo "no parts in the EFMI scene" >&2; return 1; }
	(cd "$dir" && set -o pipefail && ./harness.exe ${SECONDS_RUN:-8} --efmi efmi_list.txt "$@" 2>&1 | grep -v "^device\|^format\|^destroying\|^done")
}

cmd=$1
shift
case $cmd in
setup)
	for name in cur ref; do
		dll="$(cygpath -m "$repo/x64/Release/d3d11.dll")"
		[ $name = ref ] && dll="$(cygpath -m "$here/ref/d3d11.dll")"
		# Without extended_rendering the mods are not drawn on Vulkan, and
		# ref and cur would match for nothing
		python "$here/setup.py" EFMI --name vk_$name --dll "$dll" --api none --set Rendering.extended_rendering=1 || exit 1
		dir="$root/vk_$name"
		cp "$vk/run/harness.exe" "$vk/run/harness.pdb" "$vk/run/efmi_vs.spv" "$vk/run/efmi_ps.spv" "$vk/run/vs.spv" "$vk/run/ps.spv" "$dir/" || exit 1
		# The harness is the game, and the dump costs time
		sed -i 's/^target = .*/target = harness.exe/; s/^require_admin = .*/require_admin = false/; s/^vk_dump_hashes = .*/vk_dump_hashes = 0/' "$dir/d3dx.ini"
	done
	;;
bench)
	use_built_dll "$root/vk_cur"
	for name in ref cur; do
		echo "$name: $(run $name "$@" | tail -3 | tr '\n' ' ')"
	done
	;;
check)
	use_built_dll "$root/vk_cur"
	a=$(run ref --generated 0 "$@"); sa=$?
	b=$(run cur --generated 0 "$@"); sb=$?
	a=$(echo "$a" | grep "^middle\|^corner\|^picture checksum\|^efmi" | tr '\n' ' ')
	b=$(echo "$b" | grep "^middle\|^corner\|^picture checksum\|^efmi" | tr '\n' ' ')
	if [ $sa = 0 ] && [ $sb = 0 ] && [ "$a" = "$b" ] && [[ "$a" == *"picture checksum"* ]]; then
		echo "vulkan: same  $a"
		echo "CHECK OK"
	else
		echo "vulkan: DIFFERENT (exit codes ref $sa, cur $sb)"; echo "  ref: $a"; echo "  cur: $b"
		echo "CHECK FAILED"
		exit 1
	fi
	;;
run)
	run "$@"
	;;
*)
	sed -n '2,16p' "$0"
	exit 1
	;;
esac
