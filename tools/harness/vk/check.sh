#!/bin/bash
# check.sh [tests] [fg]: the checks of the Vulkan side, in the run folder
# setup.sh makes (default: both). Exits with 1 when one fails.
#   tests  every case/tests/test_*.ini and bench_*.ini drawn with the DLL of run
#          and with that of ref (setup.sh ref <older d3d11.dll>): the pixels
#          and the checksum of the picture have to be the same
#   fg     the fg_*.ini with frame generation (--generated 1): the triangle of
#          the mod has to be there at the end, which a corner pixel unlike
#          that of a run without the DLL tells
#   upload test_plain with the indices copied from a staging buffer into
#          memory that is not mapped (no --pool), as a game does without
#          resizable BAR, also with --upload late, unmapped and discard: the
#          pixels have to be those of --pool; with --upload fill, srcfill and
#          lastfill those of a run without the DLL, as the indices the mod is
#          for are gone
here="$(cd "$(dirname "$0")" && pwd)"
cd "$here/run" 2>/dev/null || { echo "no run folder: run setup.sh first"; exit 1; }
ok=1

# tests keeps the DLL of run as d3d11.dll.new while ref's is in place. A check
# stopped then gets it back at its exit or, killed outright, at the next start;
# else the next one would compare ref with itself.
restore_dll() {
	[ ! -f d3d11.dll.new ] || mv -f d3d11.dll.new d3d11.dll
}
restore_dll || { echo "cannot put d3d11.dll.new back as d3d11.dll"; exit 1; }
trap restore_dll EXIT
trap 'exit 1' INT TERM HUP

picture() {
	# the lines of a run that tell its picture, and its exit code
	local out status
	out=$(./harness.exe "$@" 2>&1); status=$?
	echo "$out" | grep "^middle\|^corner\|^picture checksum\|^FAILED" | tr '\n' ' '
	echo "exit $status"
}

run_tests() {
	[ -f ../ref/d3d11.dll ] || { echo "tests: FAILED, no ref folder: setup.sh ref <older d3d11.dll>"; ok=0; return; }
	mv d3d11.dll d3d11.dll.new || { echo "tests: FAILED, cannot set the DLL of run aside"; ok=0; return; }
	local ini name want got
	for ini in ../case/tests/test_*.ini ../case/tests/bench_*.ini; do
		[ -f "$ini" ] || { echo "tests: FAILED, no test inis in case/tests"; ok=0; break; }
		name=$(basename "$ini" .ini)
		cp "$ini" Mods/test.ini
		cp ../ref/d3d11.dll d3d11.dll || { echo "tests: FAILED, cannot put the DLL of ref in place"; ok=0; break; }
		want=$(picture 2 --pool)
		cp d3d11.dll.new d3d11.dll || { echo "tests: FAILED, cannot put the DLL of run back"; ok=0; break; }
		got=$(picture 2 --pool)
		if [ "$want" = "$got" ] && echo "$got" | grep -q "^middle.*checksum.*exit 0$" &&! echo "$got" | grep -q FAILED; then
			echo "same  $name: $got"
		else
			echo "DIFF  $name: ref [$want] new [$got]"
			ok=0
		fi
	done
	restore_dll || { echo "tests: FAILED, cannot put d3d11.dll.new back as d3d11.dll"; ok=0; }
	cp ../case/tests/test_texture.ini Mods/test.ini
}

run_fg() {
	local ini name background got out status
	background=$(./harness.exe 3 --pool --generated 1 --nodll 2>&1) && background=$(echo "$background" | sed -n 's/^corner: //p') || background=
	[ -n "$background" ] || { echo "fg: FAILED, no picture without the DLL"; ok=0; return; }
	for ini in ../case/tests/fg_*.ini; do
		name=$(basename "$ini" .ini)
		cp "$ini" Mods/test.ini
		out=$(./harness.exe 3 --pool --generated 1 2>&1); status=$?
		got=$(echo "$out" | sed -n 's/^corner: //p')
		if [ $status = 0 ] && [ -n "$got" ] && [ "$got" != "$background" ]; then
			echo "kept  $name: corner $got"
		else
			echo "LOST  $name: exit $status, corner ${got:-not reported}, without the DLL $background"
			ok=0
		fi
	done
	cp ../case/tests/test_texture.ini Mods/test.ini
}

run_upload() {
	local want got how
	cp ../case/tests/test_plain.ini Mods/test.ini
	for how in plain late unmapped discard fill srcfill lastfill; do
		local args=()
		[ $how = plain ] || args=(--upload $how)
		if [ $how = fill ] || [ $how = srcfill ] || [ $how = lastfill ]; then
			want=$(picture 2 "${args[@]}" --nodll | sed 's/picture checksum [0-9a-f]* //')
		else
			want=$(picture 2 --pool | sed 's/picture checksum [0-9a-f]* //')
		fi
		got=$(picture 2 "${args[@]}" | sed 's/picture checksum [0-9a-f]* //')
		if [ "$want" = "$got" ] && echo "$got" | grep -q "^middle.*exit 0$"; then
			echo "same  upload $how: $got"
		else
			echo "DIFF  upload $how: want [$want] got [$got]"
			ok=0
		fi
	done
	cp ../case/tests/test_texture.ini Mods/test.ini
}

for what in ${@:-tests fg upload}; do
	case $what in
	tests) run_tests ;;
	fg) run_fg ;;
	upload) run_upload ;;
	*) sed -n '2,12p' "$0"; exit 1 ;;
	esac
done
[ $ok = 1 ] && echo "CHECK OK" || { echo "CHECK FAILED"; exit 1; }
