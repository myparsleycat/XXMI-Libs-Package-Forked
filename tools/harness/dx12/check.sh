#!/bin/bash
# check.sh [test] [move] [lowmip] [ref]: the checks of the D3D12 shim, in the
# run folder setup.sh makes (default: test move lowmip). Exits with 1 when one
# fails.
#   test   matching by hash (case/test.ini): which watched meshes are hidden
#   move   hashes carried along GPU copies, frame by frame (harness.cpp, move)
#   lowmip a copy of a texture without its top mip level, with
#          track_texture_updates = 1 and match_lower_mip_textures off and on
#   ref    the pictures of test and of the mod in case/Mods against those of
#          the ref folder (setup.sh ref <older d3d11.dll>)
here="$(cd "$(dirname "$0")" && pwd)"
cd "$here" || exit 1
[ -x run/harness.exe ] || { echo "no run folder: run setup.sh first"; exit 1; }
ok=1

# The state of the watched meshes the harness prints with test, in its order
expect_test="A=hidden B=hidden C=drawn control=drawn texture=hidden"
# track_texture_updates = 0 (the package's d3dx.ini): a texture written after
# it was first filled keeps the hash it had, so it no longer matches
expect_test="$expect_test texture_written=drawn texture_control=drawn"

# frame:state of mesh A with move, for the cases of the comment at move_test in
# harness.cpp, looked at some frames after each: a hash is carried when the
# copying list executes, so draws of the frame of a copy may still go by the
# old one. 1950 and 1980 last that one frame and are told apart with passes=1.
expect_move="550:hidden 650:hidden 750:hidden 850:drawn 950:drawn 1050:hidden 1150:drawn
1250:drawn 1350:drawn 1500:drawn 1630:hidden 1660:drawn 1730:drawn 1830:hidden 1920:drawn"
expect_move_one_pass="1950:hidden 1980:hidden"

# match_lower_mip_textures value:state of the full texture and of its copy
expect_lowmip="0:full=hidden,copy=drawn 1:full=hidden,copy=hidden"

watched() {
	# harness output -> name=state of each watched mesh
	sed -n 's/^  mesh *[0-9]* \(A\|B\|C\|control\|texture matches\|texture written to match\|texture control\) .* \(hidden\|drawn\)$/\1=\2/p' |
		sed 's/^texture matches=/texture=/; s/^texture written to match=/texture_written=/; s/^texture control=/texture_control=/' | tr '\n' ' ' | sed 's/ $//'
}

run_test() {
	cp case/test.ini run/Mods/test.ini
	out=$(cd run && ./harness.exe frames=900 test dump=test.bmp 2>&1); status=$?
	rm -f run/Mods/test.ini
	got=$(echo "$out" | watched)
	if [ $status = 0 ] && [ "$got" = "$expect_test" ]; then
		echo "test: OK ($got)"
	else
		echo "test: FAILED (exit $status)"; echo "  expected: $expect_test"; echo "  got:      $got"
		echo "$out" | grep -v "^  mesh" | head -5 | sed 's/^/  /'
		ok=0
	fi
}

check_move_frames() {
	local expected=$1 args=$2 pair frame captures= last=0 out status got
	for pair in $expected; do
		frame=${pair%%:*}
		captures=${captures:+$captures,}$frame
		[ "$frame" -le "$last" ] || last=$frame
	done
	out=$(cd run && ./harness.exe test move frames=$((last + 1)) capture="$captures" $args 2>&1); status=$?
	got=$(echo "$out" | awk '/^capture [0-9]+:$/ { frame=$2; sub(/:$/, "", frame) }
		/^  mesh *[0-9]+ A \(vb matches\) *(hidden|drawn)$/ { print frame ":" $NF }')
	if [ "$status" = 0 ] && [ "$(echo $got)" = "$(echo $expected)" ]; then
		return 0
	fi
	echo "  move${args:+ ($args)}: exit $status"
	echo "  expected: $(echo $expected)"
	echo "  got:      $(echo $got)"
	echo "$out" | grep 'FAILED\|unknown argument\|invalid capture' | head -5 | sed 's/^/  /'
	return 1
}

run_move() {
	local bad=0
	cp case/test.ini run/Mods/test.ini
	check_move_frames "$expect_move" "" || bad=1
	check_move_frames "$expect_move_one_pass" passes=1 || bad=1
	rm -f run/Mods/test.ini
	[ $bad = 0 ] && echo "move: OK" || { echo "move: FAILED"; ok=0; }
}

lowmip_at() {
	# state of the lowmip meshes with match_lower_mip_textures = $1
	cp run/d3dx.ini run/d3dx.ini.keep
	sed -i "s/^track_texture_updates *=.*/track_texture_updates = 1/; s/^match_lower_mip_textures *=.*/match_lower_mip_textures = $1/" run/d3dx.ini
	(cd run && ./harness.exe test lowmip frames=60 dump=lowmip.bmp 2>&1) |
		sed -n 's/^  mesh *[0-9]* lowmip \(full\|copy\) *\(hidden\|drawn\)$/\1=\2/p' | tr '\n' ',' | sed 's/,$//'
	mv run/d3dx.ini.keep run/d3dx.ini
}

run_lowmip() {
	local bad=0 pair value want got
	grep -q "^match_lower_mip_textures *=" run/d3dx.ini || { echo "lowmip: FAILED, run/d3dx.ini has no match_lower_mip_textures: run setup.sh again"; ok=0; return; }
	cp case/test.ini run/Mods/test.ini
	for pair in $expect_lowmip; do
		value=${pair%%:*}; want=${pair#*:}
		got=$(lowmip_at $value)
		[ "$got" = "$want" ] || { echo "  match_lower_mip_textures = $value: ${got:-not reported}, expected $want"; bad=1; }
	done
	rm -f run/Mods/test.ini
	[ $bad = 0 ] && echo "lowmip: OK" || { echo "lowmip: FAILED"; ok=0; }
}

picture() {
	# the lines of a run that tell its picture, without the timing; fails when
	# the harness fails or prints no checksum
	local out status
	out=$(cd $1 && ./harness.exe frames=$2 $3 dump=$4 2>&1); status=$?
	echo "$out" | grep -v "draws a frame\|of that"
	[ $status = 0 ] && echo "$out" | grep -q "^picture checksum"
}

compare() {
	# compare <name> <frames> <arguments> <dump>: the picture of run against ref
	local a b sa sb
	a=$(picture run $2 "$3" $4); sa=$?
	b=$(picture ref $2 "$3" $4); sb=$?
	if [ $sa != 0 ] || [ $sb != 0 ]; then
		echo "ref $1: FAILED, a run failed or gave no picture"
		echo "$a" | head -3 | sed 's/^/  run: /'; echo "$b" | head -3 | sed 's/^/  ref: /'; ok=0
	elif [ "$a" = "$b" ]; then
		echo "ref $1: same ($(echo "$a" | grep "^picture checksum"))"
	else
		echo "ref $1: DIFFERENT"; echo "$a"; echo "$b"; ok=0
	fi
}

run_ref() {
	[ -x ref/harness.exe ] && [ -f ref/d3d11.dll ] || { echo "ref: FAILED, no ref folder: setup.sh ref <older d3d11.dll>"; ok=0; return; }
	for dir in run ref; do
		cp case/test.ini $dir/Mods/test.ini
	done
	compare test 900 test test.bmp
	rm -f run/Mods/test.ini ref/Mods/test.ini
	compare mod 1300 "" mod.bmp
}

for what in ${@:-test move lowmip}; do
	case $what in
	test) run_test ;;
	move) run_move ;;
	lowmip) run_lowmip ;;
	ref) run_ref ;;
	*) sed -n '2,9p' "$0"; exit 1 ;;
	esac
done
[ $ok = 1 ] && echo "CHECK OK" || { echo "CHECK FAILED"; exit 1; }
