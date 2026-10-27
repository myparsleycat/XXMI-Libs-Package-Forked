#!/bin/bash
# check.sh [test] [move] [ref]: the checks of the D3D12 shim, in the run
# folder setup.sh makes (default: test move). Exits with 1 when one fails.
#   test   matching by hash (case/test.ini): which watched meshes are hidden
#   move   hashes carried along GPU copies, frame by frame (harness.cpp, move)
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

move_at() {
	# state of mesh A at frame $1 with the extra arguments $2; returns the exit
	# code of the harness, which can still fail after it printed the state
	local out status
	out=$(cd run && ./harness.exe test move frames=$(( $1 + 1 )) dump=move.bmp $2 2>&1); status=$?
	echo "$out" | sed -n 's/^  mesh *[0-9]* A (vb matches) *\(hidden\|drawn\)$/\1/p'
	return $status
}

run_move() {
	local bad=0 pair frame want got status
	cp case/test.ini run/Mods/test.ini
	for pair in $expect_move; do
		frame=${pair%%:*}; want=${pair#*:}
		got=$(move_at $frame ""); status=$?
		[ $status = 0 ] && [ "$got" = "$want" ] || { echo "  frame $frame: A ${got:-not reported}, expected $want (exit $status)"; bad=1; }
	done
	for pair in $expect_move_one_pass; do
		frame=${pair%%:*}; want=${pair#*:}
		got=$(move_at $frame passes=1); status=$?
		[ $status = 0 ] && [ "$got" = "$want" ] || { echo "  frame $frame (passes=1): A ${got:-not reported}, expected $want (exit $status)"; bad=1; }
	done
	rm -f run/Mods/test.ini
	[ $bad = 0 ] && echo "move: OK" || { echo "move: FAILED"; ok=0; }
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

for what in ${@:-test move}; do
	case $what in
	test) run_test ;;
	move) run_move ;;
	ref) run_ref ;;
	*) sed -n '2,7p' "$0"; exit 1 ;;
	esac
done
[ $ok = 1 ] && echo "CHECK OK" || { echo "CHECK FAILED"; exit 1; }
