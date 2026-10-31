#!/bin/bash
# check.sh: every mode of make_mod.py with the skin.exe arguments it is for,
# against what each has to show, then queuelife, queuegone and devicelife,
# in the run folder setup.sh makes. Exits with 1 when one fails.
cd "$(dirname "$0")" || exit 1
[ -x run/skin.exe ] || { echo "no run folder: run setup.sh first"; exit 1; }
ok=1

# mode|skin.exe arguments|the spots that have to be drawn, all others empty|the
# colour the mod's spots have to be, if any:
# square (the game's mesh), bar (the mod's, as in its file), low (the lower
# rows of the mod's bar), moved (the mod's, moved by its compute shader), second
# (the second game mesh)
cases="0||bar low
1||moved
2||bar low
3||square
4||square
5||bar low
6||bar low
7|second|bar low second
8|drawfirst|low
9|unhashed second second_at=10|bar low second
10|reuse_at=10|bar low
11|replace_at=10|bar low second
12||bar low
13||bar low|green
13|second place rs11|bar low second|green
13|noheap|bar low|green
13|noheap second place rs11|bar low second|green
14|bindings second rs11|bar low second|green
15|bindings second rs11|bar low second|green
16||bar low
17|rigid|bar low
18||square
19|rigid|bar low
20|rigid|rigid
21|unusedblend||"
# Mode 12 is a mod copying so0 where it had more vertices skinned than the
# game's buffer holds; the copy has to stay within that buffer.
# Mode 13's shaders read registers the game's root signature has nothing for:
# drawn with the game's they are orange, and magenta if what they read is wrong.
# With place the game's draws read a root constant buffer, which has to be set
# again after the root signature is changed for the mod's draw and back.
# With noheap the mod needs the first allocation in a side heap, and its
# bindings must be restored to a state with no view heap afterwards.
# Mode 8's draws do not check the index buffer, so the mod's ...IB section is
# not run: the game's draw gets the mod's skinned vertices but keeps its own
# indices, which reach the bar's lower rows only, as on D3D11.
# Mode 9's weights are copied from GPU buffers whose data has a hash, which a
# copy carries since the D3D12 shim follows them, so the square is the mod's;
# the second mesh, of as many vertices, must stay the game's.

spots() {
	# skin.exe output -> the names of the spots drawn
	sed -n 's/^ *\(.*[^ ]\) *DRAWN .*/\1/p' | sed 's/^game square$/square/; s/^mod bar where it is in its file$/bar/; s/^mod bar, its lower rows$/low/;
		s/^mod bar where its compute shader moves it$/moved/; s/^second game square$/second/; s/^mod bar moved by rigid bone$/rigid/; s/^\.\.\. with the game.s vertices: \(.*\) wrong$/\1-wrong/' |
		tr '\n' ' ' | sed 's/ $//'
}

colours() {
	# skin.exe output -> the colours of the mod's spots drawn
	sed -n 's/^ *mod bar.* DRAWN (\([0-9]*\),\([0-9]*\),\([0-9]*\))$/\1 \2 \3/p' |
		awk '{ print ($2 > 150 && $1 < 100 && $3 < 100) ? "green" : ($1 > 150 && $3 > 150 && $2 < 100) ? "magenta" : "other" }' |
		sort -u | tr '\n' ' ' | sed 's/ $//'
}

run_case() {
	local mode=$1 args=$2 want=$3 colour=$4 out status got
	out=$(bash try.sh $mode 60 "$args" 2>&1); status=$?
	got=$(echo "$out" | spots)
	[ -n "$colour" ] && want="$want $colour" && got="$got $(echo "$out" | colours)"
	# A list of ours that failed to close removes the device when executed
	if [ $status = 0 ] && [ "$got" = "$want" ] && ! echo "$out" | grep -q "failed to close\|debug layer error\|debug layer corruption"; then
		echo "mode $mode${args:+ ($args)}: OK ($got)"
	else
		echo "mode $mode${args:+ ($args)}: FAILED (exit $status), expected [$want], got [$got]"
		echo "$out" | grep "FAILED\|\*\*\*" | head -3 | sed 's/^/  /'
		ok=0
	fi
}

while IFS='|' read -r mode args want colour; do
	run_case "$mode" "$args" "$want" "$colour"
done <<< "$cases"

rm -rf run/Mods/skin
for life in queuelife queuegone devicelife; do
	out=$(cd run && ./skin.exe frames=30 $life 2>&1); status=$?
	if [ $status = 0 ] && echo "$out" | grep -q "^frame"; then
		echo "$life: OK ($(echo "$out" | grep "^$life" | head -1))"
	else
		echo "$life: FAILED (exit $status)"; echo "$out" | grep "^$life\|FAILED" | head -3 | sed 's/^/  /'
		ok=0
	fi
done
[ $ok = 1 ] && echo "CHECK OK" || { echo "CHECK FAILED"; exit 1; }
