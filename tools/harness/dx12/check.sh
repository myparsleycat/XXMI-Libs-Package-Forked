#!/bin/bash
# check.sh [test] [fold] [move] [lowmip] [ref]: the checks of the D3D12 shim, in
# the run folder setup.sh makes (default: test fold move lowmip). Exits with 1
# when one fails.
#   test   matching by hash (case/test.ini): which watched meshes are hidden
#   fold   conditions on targets in a list of a game's draw: which meshes each
#          form leaves hidden
#   move   hashes carried along GPU copies, frame by frame (harness.cpp, move)
#   lowmip a copy of a texture without its top mip level, with
#          track_texture_updates = 1 and match_lower_mip_textures off and on
#   ref    the pictures of test and of the mod in case/Mods against those of
#          the ref folder (setup.sh ref <older d3d11.dll>)
#   root-uav  game UAV ranges in root signatures 1.1/1.2, including table gaps
#   dxil-iniparams  a DXIL patch reads a 1D IniParams texture on a game draw
#   dxil-iniparams-no-srv  the same patch on a shader without any SRV declarations
#   dxil-uav-blocked  an ordinary build must refuse a writable DXIL UAV patch
#   dxil-uav-replace-blocked  also refuse a UAV injected only by replacement text
#   dxil-direct-heap-blocked  refuse quoted direct descriptor-heap handles
#   dxil-sampler  add a sampler to a game DXIL shader
#   dxil-texture-sampler  add a 2D SRV and sampler to a game DXIL shader
#   dxil-sampler-states  change its sampler state between runs
#   dxil-sampler-switch  change the sampler state between draws in a run
#   dxil-sampler-existing  an unsupported replacement of a game static sampler stays unchanged
#   custom-sampler-restore  a custom shader restores an extra sampler after drawing
#   dxil-failures  reports a failed patch for each affected shader
#   dxil-failed-group  rejects commands belonging to a failed shader patch
#   dxil-matched-group  applies commands after a successful shader patch
#   dxil-existing-uav-blocked  reject writable shader patches using a game UAV
#   dxil-existing-space-uav-blocked  also reject a game UAV outside register space 0
#   dxil-captured-shader  assemble and validate a patch of a captured pixel shader
#   dxbc-converted-pipeline  run a converted DXBC shader with a DXIL vertex shader
#   dxbc-patch-conversion  reassemble a modified DXBC shader and execute its DXIL
#   dxil-legacy  report unsupported legacy assembly patterns once per group
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

# Check that unbinding the index buffer hides the game's indexed draw.
run_null_ib() {
	cp case/test.ini run/Mods/test.ini
	sed -i '/hash = 6c953b73/{n;s/handling = skip/ib = null/;}' run/Mods/test.ini
	grep -q '^ib = null$' run/Mods/test.ini || { echo 'null_ib: FAILED to prepare test'; rm -f run/Mods/test.ini; ok=0; return; }
	out=$(cd run && ./harness.exe frames=900 test dump=null_ib.bmp 2>&1); status=$?
	rm -f run/Mods/test.ini
	got=$(echo "$out" | watched)
	if [ $status = 0 ] && [ "$got" = "$expect_test" ]; then
		echo "null_ib: OK ($got)"
	else
		echo "null_ib: FAILED (exit $status)"; echo "  expected: $expect_test"; echo "  got:      $got"
		ok=0
	fi
}

fold_form() {
	# fold_form <name> <hidden|drawn> <lines>: the state of the meshes CommandListSkin
	# hides with <lines> in place of the line of harness.ini that runs it
	local name=$1 state=$2 want out status got
	want="A=$state B=$state C=drawn control=drawn"
	python -I - case/Mods/harness.ini run/Mods/harness.ini "$3" <<'EOF'
import sys
source, target, body = sys.argv[1:]
text = open(source, encoding="utf-8", newline="").read()
assert "run = CommandListSkin" in text
text = text.replace("run = CommandListSkin", body, 1).replace("[Constants]", "[Constants]\nglobal $fold_flip = 0", 1)
common = """
[Present]
$fold_flip = 1

[ResourceFoldCopy]

[ResourceBound]
type = Texture2D
width = 16
height = 16
array = 1
mips = 1
msaa = 1
format = R8G8B8A8_UNORM

[ResourceDepthBound]
type = Texture2D
width = 16
height = 16
array = 1
mips = 1
msaa = 1
format = D32_FLOAT
"""
open(target, "w", encoding="utf-8", newline="").write(text + common)
EOF
	out=$(cd run && ./harness.exe frames=900 test dump=fold.bmp 2>&1); status=$?
	got=$(echo "$out" | watched | cut -d' ' -f1-4)
	if [ $status != 0 ] || [ "$got" != "$want" ]; then
		echo "  $name: exit $status, expected $want, got $got"
		return 1
	fi
}

# Conditions on targets, which a list of a game's draw finds as mods bound them.
# The forms that bind first show that a list does see a bound target.
run_fold() {
	local bad=0
	local skin='run = CommandListSkin'
	local work='ResourceFoldCopy = copy vs-cb0'
	cp case/test.ini run/Mods/test.ini
	fold_form plain hidden "$skin" || bad=1
	fold_form dx12 hidden "if dx12
$skin
endif" || bad=1
	fold_form not_dx12 drawn "if !dx12
$skin
endif" || bad=1
	fold_form sampler_slots drawn "if dll_sampler_slots
$skin
endif" || bad=1
	fold_form unbound_target drawn "if o3 !== null
$skin
endif" || bad=1
	fold_form unbound_is_null hidden "if o3 === null
$skin
endif" || bad=1
	fold_form unbound_depth drawn "if od !== null
$skin
endif" || bad=1
	fold_form unbound_uav drawn "if ps-u1 !== null
$skin
endif" || bad=1
	fold_form else_runs hidden "if o3 !== null
$work
else
$skin
endif" || bad=1
	fold_form elif_runs hidden "if o3 !== null
$work
elif o2 === null
$skin
endif" || bad=1
	fold_form no_match drawn "if od == 924172 && DRAW_TYPE % 2 == 0
$skin
endif" || bad=1
	fold_form not_equal hidden "if od != 924172
$skin
endif" || bad=1
	fold_form or_variable_set drawn "if o3 !== null || \$costume_mods
$work
else
$skin
endif" || bad=1
	fold_form or_variable_clear hidden "if o3 !== null || !\$costume_mods
$work
else
$skin
endif" || bad=1
	fold_form and_variable_set hidden "if o3 === null && \$costume_mods
$skin
else
$work
endif" || bad=1
	fold_form and_variable_clear drawn "if o3 === null && !\$costume_mods
$skin
else
$work
endif" || bad=1
	# The variable changes after the filter was worked out
	fold_form guard_flips hidden "if o3 !== null || \$fold_flip
$work
$skin
endif" || bad=1
	fold_form bound_target hidden "o3 = ResourceBound
if o3 !== null
$skin
endif" || bad=1
	fold_form bound_depth hidden "od = ResourceDepthBound
if od !== null
$skin
endif" || bad=1
	fold_form bound_under_condition hidden "if \$costume_mods
o3 = ResourceBound
endif
if o3 !== null
$skin
endif" || bad=1
	fold_form bound_in_called_list hidden "run = CommandListFoldBind
if o3 !== null
$skin
endif

[CommandListFoldBind]
o3 = ResourceBound" || bad=1
	fold_form bound_other_slot drawn "o2 = ResourceBound
if o3 !== null
$skin
endif" || bad=1
	rm -f run/Mods/test.ini
	cp case/Mods/harness.ini run/Mods/harness.ini
	[ $bad = 0 ] && echo "fold: OK" || { echo "fold: FAILED"; ok=0; }
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

run_root_uav() {
	local signature shader status
	for signature in rootsig11 rootsig12 rootsig11-gap; do
		for shader in game-uav-root game-shader-uav game-shader-uav-space1; do
			(cd run && ./harness.exe nodll dxil gpu-debug "$signature" "$shader" frames=4 >root-uav.log 2>&1); status=$?
			if [ "$status" != 0 ]; then
				echo "root-uav: FAILED ($signature $shader, exit $status)"
				cat run/root-uav.log; ok=0; return
			fi
		done
	done
	echo 'root-uav: OK (9 combinations, GPU validation)'
}

run_dxil_iniparams() {
	local a b args=""
	cp case/dxil-iniparams.ini run/Mods/dxil-iniparams.ini || return 1
	if [ "$1" = no-srv ]; then
		sed -i '/^dx12_match_registers = /d' run/Mods/dxil-iniparams.ini
		args=no-srv
	fi
	(cd run && ./harness.exe dxil $args frames=60 dump=dxil-iniparams-on.bmp >/dev/null 2>&1); a=$?
	rm -f run/Mods/dxil-iniparams.ini
	(cd run && ./harness.exe dxil $args frames=60 dump=dxil-iniparams-off.bmp >/dev/null 2>&1); b=$?
	if [ "$a" = 0 ] && [ "$b" = 0 ] && python check_dxil_iniparams.py run/dxil-iniparams-on.bmp run/dxil-iniparams-off.bmp; then
		echo "dxil-iniparams${1:+-$1}: OK"
	else
		echo "dxil-iniparams${1:+-$1}: FAILED (exit $a, $b)"; ok=0
	fi
}

run_dxil_failures() {
	local status count
	cp run/d3dx.ini run/d3dx.ini.dxil-failures-backup || { ok=0; return; }
	sed -i 's/^;dx12_dump_hashes = 1/dx12_dump_hashes = 1/' run/d3dx.ini
	cp case/dxil-failure.ini run/Mods/dxil-failure.ini
	(cd run && ./harness.exe dxil frames=60 >/dev/null 2>&1); status=$?
	rm -f run/Mods/dxil-failure.ini
	mv run/d3dx.ini.dxil-failures-backup run/d3dx.ini
	count=$(grep -c '^DXIL ShaderRegex patch failed for ' run/dx12_report.txt || true)
	if [ "$status" = 0 ] && [ "$count" -ge 2 ]; then
		echo "dxil-failures: OK ($count shaders reported)"
	else
		echo "dxil-failures: FAILED (exit $status, $count shaders reported)"; ok=0
	fi
}

run_dxil_failed_group() {
	local a b
	cp case/dxil-failure.ini run/Mods/dxil-failure.ini || { ok=0; return; }
	sed -i '/^dx12_match_registers = /a handling = skip' run/Mods/dxil-failure.ini
	(cd run && ./harness.exe dxil frames=4 dump=dxil-failed-group-on.bmp >/dev/null 2>&1); a=$?
	rm -f run/Mods/dxil-failure.ini
	(cd run && ./harness.exe dxil frames=4 dump=dxil-failed-group-off.bmp >/dev/null 2>&1); b=$?
	if [ "$a" = 0 ] && [ "$b" = 0 ] && cmp -s run/dxil-failed-group-on.bmp run/dxil-failed-group-off.bmp; then
		echo 'dxil-failed-group: OK'
	else
		echo "dxil-failed-group: FAILED (exit $a, $b)"; ok=0
	fi
}

run_dxil_matched_group() {
	local a b
	cp case/dxil-iniparams.ini run/Mods/dxil-iniparams.ini || { ok=0; return; }
	sed -i '/^dx12_match_registers = /a handling = skip' run/Mods/dxil-iniparams.ini
	sed -i 's/^x172 = 0.5$/x172 = 0/' run/Mods/dxil-iniparams.ini
	(cd run && ./harness.exe dxil frames=4 dump=dxil-matched-group-on.bmp >/dev/null 2>&1); a=$?
	rm -f run/Mods/dxil-iniparams.ini
	(cd run && ./harness.exe dxil frames=4 dump=dxil-matched-group-off.bmp >/dev/null 2>&1); b=$?
	if [ "$a" = 0 ] && [ "$b" = 0 ] && ! cmp -s run/dxil-matched-group-on.bmp run/dxil-matched-group-off.bmp; then
		echo 'dxil-matched-group: OK'
	else
		echo "dxil-matched-group: FAILED (exit $a, $b)"; ok=0
	fi
}

run_dxil_captured_shader() {
	local status baseline on_sum off_sum report=run/dx12_report.txt
	[ -n "${DX12_PROBE_SHADER:-}" ] && [ -f "$DX12_PROBE_SHADER" ] || { echo 'dxil-captured-shader: set DX12_PROBE_SHADER'; ok=0; return; }
	cp run/d3dx.ini run/d3dx.ini.probe-backup || { ok=0; return; }
	sed -i 's/^;dx12_dump_hashes = 1/dx12_dump_hashes = 1/' run/d3dx.ini
	cp "$DX12_PROBE_SHADER" run/probe-ps.dxil
	cp case/dxil-iniparams.ini run/Mods/dxil-iniparams.ini
	# Match the supplied shader's entry regardless of its name.
	sed -i 's/@ps/@[^ (]+/' run/Mods/dxil-iniparams.ini
	(cd run && ./harness.exe dxil gpu-debug frames=4 probe-shader=probe-ps.dxil >dxil-probe.log 2>&1); status=$?
	cp "$report" run/dxil-probe-report.txt
	rm -f run/Mods/dxil-iniparams.ini
	(cd run && ./harness.exe dxil gpu-debug frames=4 probe-shader=probe-ps.dxil >dxil-probe-baseline.log 2>&1); baseline=$?
	on_sum=$(awk '/^captured shader output / {print $4}' run/dxil-probe.log)
	off_sum=$(awk '/^captured shader output / {print $4}' run/dxil-probe-baseline.log)
	rm -f run/probe-ps.dxil
	mv run/d3dx.ini.probe-backup run/d3dx.ini
	if [ "$status" = 0 ] && [ "$baseline" = 0 ] && [ -n "$on_sum" ] && [ -n "$off_sum" ] && [ "$on_sum" != "$off_sum" ] \
	   && grep -Eq 'DXIL ShaderRegex patch succeeded for [[:xdigit:]]{16}' run/dxil-probe-report.txt; then
		echo "dxil-captured-shader: OK"
	else
		echo "dxil-captured-shader: FAILED (exit $status, $baseline; output $on_sum, $off_sum)"; ok=0
		grep -E 'FAILED|D3D12 debug error' run/dxil-probe.log | head -5
		grep 'DXIL ShaderRegex patch failed for ' run/dxil-probe-report.txt | head -1
	fi
}

run_dxbc_converted_pipeline() {
	local probe status
	(cd run && ./harness.exe probe-dxbc-conversion >dxbc-conversion.log 2>&1); probe=$?
	(cd run && ./harness.exe converted-ps gpu-debug frames=4 dump=dxbc-converted.bmp >>dxbc-conversion.log 2>&1); status=$?
	if [ "$probe" = 0 ] && [ "$status" = 0 ] && grep -q '^DXBC converted to DXIL container' run/dxbc-conversion.log && [ -s run/dxbc-converted.bmp ]; then
		echo 'dxbc-converted-pipeline: OK'
	else
		echo "dxbc-converted-pipeline: FAILED (exit $probe, $status)"; ok=0
		grep -E 'FAILED|D3D12 debug error|invalid|unavailable' run/dxbc-conversion.log | head -5
	fi
}

run_dxbc_patch_conversion() {
	local a b c d
	[ -x bin/probe_assemble.exe ] || { echo 'dxbc-patch-conversion: run build-dxbc-probe.bat first'; ok=0; return; }
	cp bin/probe_assemble.exe run/probe_assemble.exe
	(cd run && ./harness.exe probe-dxbc-conversion >dxbc-patch.log 2>&1); a=$?
	(cd run && ./probe_assemble.exe >>dxbc-patch.log 2>&1); b=$?
	(cd run && ./harness.exe converted-ps-file=probe-dxbc-patched.bin gpu-debug frames=4 dump=dxbc-patch-on.bmp >>dxbc-patch.log 2>&1); c=$?
	(cd run && ./harness.exe converted-ps-file=probe-dxbc.bin gpu-debug frames=4 dump=dxbc-patch-off.bmp >>dxbc-patch.log 2>&1); d=$?
	if [ "$a" = 0 ] && [ "$b" = 0 ] && [ "$c" = 0 ] && [ "$d" = 0 ] \
	   && python check_dxil_iniparams.py run/dxbc-patch-on.bmp run/dxbc-patch-off.bmp; then
		echo 'dxbc-patch-conversion: OK'
	else
		echo "dxbc-patch-conversion: FAILED (exit $a, $b, $c, $d)"; ok=0
		grep -E 'FAILED|D3D12 debug error|missing|unavailable' run/dxbc-patch.log | head -5
	fi
}

run_dxil_legacy() {
	local status count
	cp run/d3dx.ini run/d3dx.ini.legacy-backup || { ok=0; return; }
	sed -i 's/^;dx12_dump_hashes = 1/dx12_dump_hashes = 1/' run/d3dx.ini
	cp case/dxil-legacy.ini run/Mods/dxil-legacy.ini
	(cd run && ./harness.exe dxil frames=60 >/dev/null 2>&1); status=$?
	rm -f run/Mods/dxil-legacy.ini
	mv run/d3dx.ini.legacy-backup run/d3dx.ini
	count=$(grep -ic '^DXIL does not apply legacy.*LegacyDiagnostic' run/dx12_report.txt || true)
	if [ "$status" = 0 ] && [ "$count" = 1 ]; then
		echo 'dxil-legacy: OK'
	else
		echo "dxil-legacy: FAILED (exit $status, $count reports)"; ok=0
	fi
}

run_dxil_uav_blocked() {
	local a b
	cp run/d3dx.ini run/d3dx.ini.uav-blocked-backup || { ok=0; return; }
	sed -i 's/^;dx12_dump_hashes = 1/dx12_dump_hashes = 1/' run/d3dx.ini
	cp case/dxil-uav.ini run/Mods/dxil-uav.ini
	sed -i '/^dx12_match_registers = /a handling = skip' run/Mods/dxil-uav.ini
	(cd run && ./harness.exe dxil game-uav-root frames=60 dump=dxil-uav-blocked.bmp >dxil-uav-blocked.log 2>&1); a=$?
	cp run/dx12_report.txt run/dxil-uav-blocked-report.txt
	rm -f run/Mods/dxil-uav.ini
	(cd run && ./harness.exe dxil game-uav-root frames=60 dump=dxil-uav-plain.bmp >>dxil-uav-blocked.log 2>&1); b=$?
	mv run/d3dx.ini.uav-blocked-backup run/d3dx.ini
	if [ "$a" = 0 ] && [ "$b" = 0 ] && cmp -s run/dxil-uav-blocked.bmp run/dxil-uav-plain.bmp \
	   && grep -q 'writable DXIL UAVs are not enabled in this build' run/dxil-uav-blocked-report.txt; then
		echo 'dxil-uav-blocked: OK'
	else
		echo "dxil-uav-blocked: FAILED (exit $a, $b)"; ok=0
	fi
}

run_dxil_unsafe_replace_blocked() {
	local name=$1 fixture=$2 expected=$3 a b mod=run/Mods/mod/mod.ini
	cp run/d3dx.ini run/d3dx.ini.unsafe-replace-backup || { ok=0; return; }
	sed -i 's/^;dx12_dump_hashes = 1/dx12_dump_hashes = 1/' run/d3dx.ini
	mv "$mod" "$mod.unsafe-replace-backup" || { ok=0; return; }
	cp "case/$fixture" "run/Mods/$fixture" || {
		mv "$mod.unsafe-replace-backup" "$mod"; mv run/d3dx.ini.unsafe-replace-backup run/d3dx.ini; ok=0; return;
	}
	(cd run && ./harness.exe dxil frames=4 dump="$name-on.bmp" >"$name.log" 2>&1); a=$?
	cp run/dx12_report.txt "run/$name-report.txt"
	rm -f "run/Mods/$fixture"
	(cd run && ./harness.exe dxil frames=4 dump="$name-off.bmp" >>"$name.log" 2>&1); b=$?
	mv "$mod.unsafe-replace-backup" "$mod"
	mv run/d3dx.ini.unsafe-replace-backup run/d3dx.ini
	if [ "$a" = 0 ] && [ "$b" = 0 ] && cmp -s "run/$name-on.bmp" "run/$name-off.bmp" \
	   && grep -q "$expected" "run/$name-report.txt"; then
		echo "$name: OK"
	else
		echo "$name: FAILED (exit $a, $b)"; ok=0
		grep 'DXIL ShaderRegex patch failed' "run/$name-report.txt" | head -2
	fi
}

run_dxil_existing_uav_blocked() {
	local a b
	cp run/d3dx.ini run/d3dx.ini.existing-uav-backup || { ok=0; return; }
	sed -i 's/^;dx12_dump_hashes = 1/dx12_dump_hashes = 1/' run/d3dx.ini
	cp case/dxil-iniparams.ini run/Mods/dxil-iniparams.ini
	(cd run && ./harness.exe dxil game-shader-uav frames=4 dump=dxil-existing-uav-on.bmp >dxil-existing-uav.log 2>&1); a=$?
	cp run/dx12_report.txt run/dxil-existing-uav-report.txt
	rm -f run/Mods/dxil-iniparams.ini
	(cd run && ./harness.exe dxil game-shader-uav frames=4 dump=dxil-existing-uav-off.bmp >>dxil-existing-uav.log 2>&1); b=$?
	mv run/d3dx.ini.existing-uav-backup run/d3dx.ini
	if [ "$a" = 0 ] && [ "$b" = 0 ] && cmp -s run/dxil-existing-uav-on.bmp run/dxil-existing-uav-off.bmp \
	   && grep -q 'writable DXIL UAVs are not enabled in this build' run/dxil-existing-uav-report.txt; then
		echo 'dxil-existing-uav-blocked: OK'
	else
		echo "dxil-existing-uav-blocked: FAILED (exit $a, $b)"; ok=0
	fi
}

run_dxil_existing_space_uav_blocked() {
	local a b
	cp run/d3dx.ini run/d3dx.ini.existing-space-uav-backup || { ok=0; return; }
	sed -i 's/^;dx12_dump_hashes = 1/dx12_dump_hashes = 1/' run/d3dx.ini
	cp case/dxil-iniparams.ini run/Mods/dxil-iniparams.ini
	(cd run && ./harness.exe dxil game-shader-uav-space1 frames=4 dump=dxil-existing-space-uav-on.bmp >dxil-existing-space-uav.log 2>&1); a=$?
	cp run/dx12_report.txt run/dxil-existing-space-uav-report.txt
	rm -f run/Mods/dxil-iniparams.ini
	(cd run && ./harness.exe dxil game-shader-uav-space1 frames=4 dump=dxil-existing-space-uav-off.bmp >>dxil-existing-space-uav.log 2>&1); b=$?
	mv run/d3dx.ini.existing-space-uav-backup run/d3dx.ini
	if [ "$a" = 0 ] && [ "$b" = 0 ] && cmp -s run/dxil-existing-space-uav-on.bmp run/dxil-existing-space-uav-off.bmp \
	   && grep -q 'writable DXIL UAVs are not enabled in this build' run/dxil-existing-space-uav-report.txt; then
		echo 'dxil-existing-space-uav-blocked: OK'
	else
		echo "dxil-existing-space-uav-blocked: FAILED (exit $a, $b)"; ok=0
	fi
}

run_dxil_sampler() {
	local a b debug="" frames=60 args=""
	[ "${DX12_DEBUG:-0}" = 1 ] && debug=debug
	if [ "${DX12_GPU_DEBUG:-0}" = 1 ]; then debug=gpu-debug; frames=4; fi
	[ "${DX12_ROOTSIG11:-0}" = 1 ] && args=rootsig11
	[ "${DX12_ROOTSIG_GAP:-0}" = 1 ] && args=rootsig11-gap
	[ "${DX12_ROOTSIG12:-0}" = 1 ] && args=rootsig12
	[ "${DX12_HEAP_FULL:-0}" = 1 ] && args="$args heap=1000000"
	cp case/dxil-sampler.ini run/Mods/dxil-sampler.ini || { ok=0; return; }
	if [ "$1" = texture ]; then
		sed -i '/^dx12_match_registers = /a ps-t60 = ResourceDxilTexture' run/Mods/dxil-sampler.ini
		sed -i '/^dcl_sampler s15, mode_default$/a dcl_resource_texture2d (float,float,float,float) t60' run/Mods/dxil-sampler.ini
		sed -i '/^%zz_sample = /s/${texture}/%zz_d3dmigoto_t60/' run/Mods/dxil-sampler.ini
		printf '\n[ResourceDxilTexture]\nfilename = mod/test.dds\n' >> run/Mods/dxil-sampler.ini
	fi
	(cd run && ./harness.exe dxil $debug $args frames=$frames dump=dxil-sampler-on.bmp >dxil-sampler-debug.log 2>&1); a=$?
	rm -f run/Mods/dxil-sampler.ini
	(cd run && ./harness.exe dxil $debug $args frames=$frames dump=dxil-sampler-off.bmp >>dxil-sampler-debug.log 2>&1); b=$?
	if [ "$a" = 0 ] && [ "$b" = 0 ] && python check_dxil_iniparams.py run/dxil-sampler-on.bmp run/dxil-sampler-off.bmp; then
		echo "dxil-${1:+$1-}sampler: OK"
	else
		echo "dxil-${1:+$1-}sampler: FAILED (exit $a, $b)"; ok=0
		grep '^D3D12 debug error:' run/dxil-sampler-debug.log | head -5
	fi
}

run_dxil_sampler_states() {
	local a b debug="" frames=60
	if [ "${DX12_GPU_DEBUG:-0}" = 1 ]; then debug=gpu-debug; frames=4; fi
	cp case/dxil-sampler.ini run/Mods/dxil-sampler.ini || { ok=0; return; }
	sed -i '/^dx12_match_registers = /a ps-s15 = ResourceDxilSampler' run/Mods/dxil-sampler.ini
	printf '\n[ResourceDxilSampler]\ntype = SamplerState\nfilter = point\naddress = wrap\n' >> run/Mods/dxil-sampler.ini
	if [ "$1" = switch ]; then
		printf '\n[ResourceDxilSamplerLinear]\ntype = SamplerState\nfilter = linear\naddress = clamp\n\n[Constants]\nglobal $sampler_test_toggle = 0\n' >> run/Mods/dxil-sampler.ini
	fi
	(cd run && ./harness.exe dxil $debug frames=$frames dump=dxil-sampler-point.bmp >dxil-sampler-states.log 2>&1); a=$?
	if [ "$1" = switch ]; then
		sed -i '/^ps-s15 = ResourceDxilSampler$/c\if $sampler_test_toggle == 0\nps-s15 = ResourceDxilSampler\nelse\nps-s15 = ResourceDxilSamplerLinear\nendif\n$sampler_test_toggle = 1 - $sampler_test_toggle' run/Mods/dxil-sampler.ini
	else
		sed -i 's/^filter = point$/filter = linear/; s/^address = wrap$/address = clamp/' run/Mods/dxil-sampler.ini
	fi
	(cd run && ./harness.exe dxil $debug frames=$frames dump=dxil-sampler-linear.bmp >>dxil-sampler-states.log 2>&1); b=$?
	rm -f run/Mods/dxil-sampler.ini
	if [ "$a" = 0 ] && [ "$b" = 0 ] && python check_sampler_difference.py run/dxil-sampler-point.bmp run/dxil-sampler-linear.bmp; then
		echo "dxil-sampler-${1:-states}: OK"
	else
		echo "dxil-sampler-${1:-states}: FAILED (exit $a, $b)"; ok=0
		grep '^D3D12 debug error:' run/dxil-sampler-states.log | head -5
	fi
}

run_dxil_sampler_existing() {
	local a b debug="" frames=60
	if [ "${DX12_GPU_DEBUG:-0}" = 1 ]; then debug=gpu-debug; frames=4; fi
	cp case/dxil-sampler.ini run/Mods/dxil-sampler.ini || { ok=0; return; }
	(cd run && ./harness.exe dxil $debug frames=$frames dump=dxil-sampler-existing-off.bmp >dxil-sampler-existing.log 2>&1); a=$?
	sed -i '/^dx12_match_registers = /a ps-s0 = ResourceDxilSampler' run/Mods/dxil-sampler.ini
	printf '\n[ResourceDxilSampler]\ntype = SamplerState\nfilter = linear\naddress = clamp\n' >> run/Mods/dxil-sampler.ini
	(cd run && ./harness.exe dxil $debug frames=$frames dump=dxil-sampler-existing-on.bmp >>dxil-sampler-existing.log 2>&1); b=$?
	rm -f run/Mods/dxil-sampler.ini
	if [ "$a" = 0 ] && [ "$b" = 0 ] && cmp -s run/dxil-sampler-existing-off.bmp run/dxil-sampler-existing-on.bmp; then
		echo 'dxil-sampler-existing: OK'
	else
		echo "dxil-sampler-existing: FAILED (exit $a, $b)"; ok=0
		grep '^D3D12 debug error:' run/dxil-sampler-existing.log | head -5
	fi
}

run_custom_sampler_restore() {
	local mod=run/Mods/mod/mod.ini a b debug="" frames=60
	if [ "${DX12_GPU_DEBUG:-0}" = 1 ]; then debug=gpu-debug; frames=4; fi
	cp "$mod" "$mod.sampler-custom-backup" || { ok=0; return; }
	sed -i '/^\[TextureOverrideMesh100\]/,/^\[/{ /^ps-t0 = ResourceModDiffuse$/d; s/^drawindexed = auto$/run = CommandListSamplerCustom/; }' "$mod"
	cat case/sampler-custom.ini >> "$mod"
	cp case/sampler-custom-*.hlsl run/Mods/mod/
	(cd run && ./harness.exe dxil $debug frames=$frames dump=sampler-custom-explicit.bmp >sampler-custom.log 2>&1); a=$?
	sed -i '/^ps-s15 = ResourceSamplerLinear$/d' "$mod"
	(cd run && ./harness.exe dxil $debug frames=$frames dump=sampler-custom-restored.bmp >>sampler-custom.log 2>&1); b=$?
	mv "$mod.sampler-custom-backup" "$mod"
	rm -f run/Mods/mod/sampler-custom-*.hlsl
	if [ "$a" = 0 ] && [ "$b" = 0 ] && cmp -s run/sampler-custom-explicit.bmp run/sampler-custom-restored.bmp; then
		echo 'custom-sampler-restore: OK'
	else
		echo "custom-sampler-restore: FAILED (exit $a, $b)"; ok=0
		grep '^D3D12 debug error:' run/sampler-custom.log | head -5
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

for what in ${@:-test fold move lowmip}; do
	case $what in
	test) run_test; run_null_ib ;;
	fold) run_fold ;;
	move) run_move ;;
	lowmip) run_lowmip ;;
	ref) run_ref ;;
	root-uav) run_root_uav ;;
	dxil-iniparams) run_dxil_iniparams '' ;;
	dxil-iniparams-no-srv) run_dxil_iniparams no-srv ;;
	dxil-failures) run_dxil_failures ;;
	dxil-failed-group) run_dxil_failed_group ;;
	dxil-matched-group) run_dxil_matched_group ;;
	dxil-captured-shader) run_dxil_captured_shader ;;
	dxbc-converted-pipeline) run_dxbc_converted_pipeline ;;
	dxbc-patch-conversion) run_dxbc_patch_conversion ;;
	dxil-legacy) run_dxil_legacy ;;
	dxil-uav-blocked) run_dxil_uav_blocked ;;
	dxil-uav-replace-blocked) run_dxil_unsafe_replace_blocked "$what" dxil-uav-replace.ini 'patched shader has a writable UAV' ;;
	dxil-direct-heap-blocked) run_dxil_unsafe_replace_blocked "$what" dxil-direct-heap-replace.ini 'directly indexed DXIL descriptor heaps' ;;
	dxil-existing-uav-blocked) run_dxil_existing_uav_blocked ;;
	dxil-existing-space-uav-blocked) run_dxil_existing_space_uav_blocked ;;
	dxil-sampler) run_dxil_sampler '' ;;
	dxil-texture-sampler) run_dxil_sampler texture ;;
	dxil-sampler-states) run_dxil_sampler_states '' ;;
	dxil-sampler-switch) run_dxil_sampler_states switch ;;
	dxil-sampler-existing) run_dxil_sampler_existing ;;
	custom-sampler-restore) run_custom_sampler_restore ;;
	*) sed -n '2,/^[^#]/{ /^[^#]/!p; }' "$here/check.sh"; exit 1 ;;
	esac
done
[ $ok = 1 ] && echo "CHECK OK" || { echo "CHECK FAILED"; exit 1; }
