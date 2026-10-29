#!/bin/bash
# The harness for the CPU side of the DLL, run on the deployed configuration of
# an importer (GIMI SRMI ZZMI WWMI EFMI) with every mod the game has active.
#
#   xh.sh setup [importer...]     the run folders: "cur" with the DLL of x64/Release,
#                                 "ref" with ref/d3d11.dll (the one results are compared with)
#   xh.sh ref                     makes the DLL of x64/Release the reference
#   xh.sh check [importer...]     the same frames through ref and cur: what reached
#                                 Direct3D must be the same call for call, frame by frame
#   xh.sh bench [importer...]     time a frame on the game's thread: without the DLL, ref, cur
#   xh.sh profile <importer> [harness arguments]   where cur spends that time
#   xh.sh diff <importer> <frame>                  the calls of a frame, ref against cur
#   xh.sh run <importer> <cur|ref> [harness arguments]
#
# check, bench, profile and diff copy the DLL of x64/Release into cur first.
#
# RUNS=5 xh.sh bench GIMI repeats more; CHECK_FRAMES, CHECK_ARGS, BENCH_FRAMES,
# BENCH_ARGS and LINES_SHOWN (diff) likewise. XXMI_ROOT is the folder with the
# importer folders, XH_RUNS where the run folders go (default ~/.xh).
# check exits with 1 when a run differs or fails.
here="$(cd "$(dirname "$0")" && pwd)"
repo="$here/../../.."
runs_root="${XH_RUNS:-$HOME/.xh}"
all="GIMI SRMI ZZMI WWMI EFMI"

scene() { cygpath -m "$here/scenes/$1.scene"; }

use_built_dll() {
	# check, bench, profile and diff test the DLL just built: it goes into the
	# cur folder $1 first. run takes the folder as it is (ab.sh XH_DLL).
	[ -d "$1" ] || return 0
	cp "$repo/x64/Release/d3d11.dll" "$1/d3d11.dll" || exit 1
	rm -f "$1/d3d11.pdb"
	[ -f "$repo/x64/Release/d3d11.pdb" ] && cp "$repo/x64/Release/d3d11.pdb" "$1/d3d11.pdb"
}

# What a frame of each importer is made of, roughly as its game draws
bench_args() {
	case $1 in
	GIMI) echo "chars=6 scenery=700 passes=3" ;;
	SRMI) echo "chars=6 scenery=500 passes=3" ;;
	ZZMI) echo "chars=5 scenery=350 passes=3" ;;
	WWMI) echo "chars=5 scenery=600 passes=3" ;;
	EFMI) echo "chars=6 scenery=500 passes=4" ;;
	esac
}

run() {
	local importer=$1 name=$2
	shift 2
	local dir="$runs_root/$importer/$name" ini=game
	[[ " $* " == *" check "* ]] && ini=check
	[ -f "$dir/d3dx.ini.$ini" ] && cp "$dir/d3dx.ini.$ini" "$dir/d3dx.ini"
	# No .start: a folder of an older setup.py whose game had no settings file
	if [ -f "$dir/d3dx_user.ini.start" ]; then
		cp "$dir/d3dx_user.ini.start" "$dir/d3dx_user.ini"
	else
		rm -f "$dir/d3dx_user.ini"
	fi
	(cd "$dir" && ./dx11_harness.exe scene="$(scene $importer)" "$@")
}

cmd=$1
shift
case $cmd in
setup)
	mkdir -p "$here/scenes"
	for i in ${@:-$all}; do
		python "$here/scene.py" $i --out "$here/scenes/$i.scene" || exit 1
		python "$here/setup.py" $i --name cur || exit 1
		if [ -f "$here/ref/d3d11.dll" ]; then
			python "$here/setup.py" $i --name ref --dll "$(cygpath -m "$here/ref/d3d11.dll")" || exit 1
		fi
	done
	;;
ref)
	mkdir -p "$here/ref"
	cp "$repo/x64/Release/d3d11.dll" "$repo/x64/Release/d3d11.pdb" "$here/ref/"
	(cd "$repo" && git rev-parse --short HEAD && git status --short | head -20) > "$here/ref/what.txt"
	echo "reference is now the DLL of x64/Release ($(head -1 "$here/ref/what.txt"))"
	;;
check)
	ok=1
	for i in ${@:-$all}; do
		frames=${CHECK_FRAMES:-360}
		for which in ref cur; do
			[ -x "$runs_root/$i/$which/dx11_harness.exe" ] || { echo "$i: no $which run folder, run xh.sh setup $i (and xh.sh ref) first"; exit 1; }
			rm -f "$runs_root/$i/$which/frames.txt"
		done
		use_built_dll "$runs_root/$i/cur"
		a=$(run $i ref frames=$frames check rotate=6 $CHECK_ARGS 2>&1); sa=$?
		b=$(run $i cur frames=$frames check rotate=6 $CHECK_ARGS 2>&1); sb=$?
		a=$(echo "$a" | tail -1)
		b=$(echo "$b" | tail -1)
		ua=$(md5sum < "$runs_root/$i/ref/d3dx_user.ini" 2>/dev/null)
		ub=$(md5sum < "$runs_root/$i/cur/d3dx_user.ini" 2>/dev/null)
		if [ $sa != 0 ] || [ $sb != 0 ] || [ ! -s "$runs_root/$i/cur/frames.txt" ]; then
			ok=0
			echo "$i: FAILED (exit codes ref $sa, cur $sb)"
			echo "  ref: $a"
			echo "  cur: $b"
		elif cmp -s "$runs_root/$i/ref/frames.txt" "$runs_root/$i/cur/frames.txt" && [ "$a" = "$b" ] && [ "$ua" = "$ub" ]; then
			echo "$i: same  $a"
		else
			ok=0
			first=$(diff "$runs_root/$i/ref/frames.txt" "$runs_root/$i/cur/frames.txt" | sed -n 's/^< \([0-9]*\) .*/\1/p' | head -1)
			echo "$i: DIFFERENT from frame ${first:-?}"
			echo "  ref: $a"
			echo "  cur: $b"
			[ "$ua" = "$ub" ] || echo "  d3dx_user.ini differs too"
			# Only the state bound at draws and the like, or the order of calls as well?
			awk '{print $1, $3}' "$runs_root/$i/ref/frames.txt" > "$runs_root/$i/e_ref.txt"
			awk '{print $1, $3}' "$runs_root/$i/cur/frames.txt" > "$runs_root/$i/e_cur.txt"
			cmp -s "$runs_root/$i/e_ref.txt" "$runs_root/$i/e_cur.txt" && echo "  (what was drawn with what is the same; only calls that set state differ)"
		fi
	done
	[ $ok = 1 ] && echo "CHECK OK" || { echo "CHECK FAILED"; exit 1; }
	;;
bench)
	n=${RUNS:-3}
	for i in ${@:-$all}; do
		args="frames=${BENCH_FRAMES:-900} warmup=300 $(bench_args $i) $BENCH_ARGS"
		use_built_dll "$runs_root/$i/cur"
		for which in nodll ref cur; do
			dir=$which
			extra=
			[ $which = nodll ] && { dir=cur; extra=nodll; }
			[ -d "$runs_root/$i/$dir" ] || continue
			values=$(for k in $(seq $n); do run $i $dir $args $extra 2>&1 | sed -n 's/^frame \([0-9.]*\) ms median.*/\1/p'; done)
			echo "$i $which: $(echo "$values" | sort -n | sed -n "$(( (n + 1) / 2 ))p") ms a frame   (all: $(echo $values))"
		done
	done
	;;
profile)
	i=$1
	shift
	use_built_dll "$runs_root/$i/cur"
	run $i cur frames=${BENCH_FRAMES:-1500} warmup=300 $(bench_args $i) $BENCH_ARGS profile "$@"
	;;
diff)
	i=$1
	use_built_dll "$runs_root/$i/cur"
	for which in ref cur; do
		run $i $which frames=$(( $2 + 1 )) check rotate=6 $CHECK_ARGS text=$2-$2 > /dev/null 2>&1
		grep -v '^made\|^unknown' "$runs_root/$i/$which/trace.txt" > "$runs_root/$i/trace_$which.txt"
	done
	diff "$runs_root/$i/trace_ref.txt" "$runs_root/$i/trace_cur.txt" | head -${LINES_SHOWN:-40}
	;;
run)
	run "$@"
	;;
*)
	sed -n '2,21p' "$0"
	exit 1
	;;
esac
