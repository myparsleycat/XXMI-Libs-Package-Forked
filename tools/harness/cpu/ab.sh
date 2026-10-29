#!/bin/bash
# ab.sh <line> <runs> "<env of variant A>" "<env of variant B>" ...
# The first-appearance runs of a line, the variants taken in turn, and the
# median of each variant's "first frame over steady" total, its whole
# APPEAR total (first three frames) and its worst first frame.
#   line: dx11:<importer> | dx12:<importer> | vk
# An empty variant ("") is the DLL as it is. Variables of a variant are
# separated by spaces, e.g. "XH_LOAD=read XH_NOTRESIDENT=1". XH_DLL=<path>
# copies that DLL into the line's run folder before each of its runs; the
# other variants run the DLL the folder had at the start, which it gets back
# at the end.
export here="$(cd "$(dirname "$0")" && pwd)"
export runs_root="${XH_RUNS:-$HOME/.xh}"
export line=$1
runs=$2
shift 2

case $line in
dx11:*) d=${line#dx11:}/cur ;;
dx12:*) d=${line#dx12:}/dx12_cur ;;
vk) d=EFMI/vk_cur ;;
*) sed -n '2,11p' "$0"; exit 1 ;;
esac
export dll="$runs_root/$d/d3d11.dll"
export base_dll="$(mktemp)"
cp "$dll" "$base_dll" || { rm -f "$base_dll"; exit 1; }
trap 'cp "$base_dll" "$dll"; rm -f "$base_dll"' EXIT

run_once() {
	cp "${XH_DLL:-$base_dll}" "$dll" || return
	case $line in
	dx11:*) bash "$here/xh.sh" run ${line#dx11:} cur appear chars=1 rotate=12 warmup=60 scenery=700 passes=3 2>&1 ;;
	dx12:*) MODS=200 PARTS=5000 bash "$here/dx12.sh" run ${line#dx12:} cur appear=12 2>&1 ;;
	# --appear ends the run itself, 600 s is only the ceiling
	vk) SECONDS_RUN=600 bash "$here/vk.sh" run cur --appear 12 2>&1 ;;
	esac
}

declare -A first total worst
for r in $(seq $runs); do
	for v in "$@"; do
		out=$(env $v bash -c "$(declare -f run_once); run_once")
		s=$?
		if [ $s != 0 ] || [[ "$out" != *"first frame over steady"* ]]; then
			echo "$out" >&2
			echo "run $r [${v:-as is}] failed (exit code $s) or gave no appear numbers" >&2
			exit 1
		fi
		f=$(echo "$out" | sed -n 's/.*first frame over steady: total \([0-9.-]*\) ms.*/\1/p')
		w=$(echo "$out" | sed -n 's/.*first frame over steady:.* max \([0-9.-]*\).*/\1/p')
		t=$(echo "$out" | sed -n 's/^APPEAR total \([0-9.-]*\) ms.*/\1/p')
		first[k:$v]="${first[k:$v]} $f"
		total[k:$v]="${total[k:$v]} $t"
		worst[k:$v]="${worst[k:$v]} $w"
		echo "run $r [${v:-as is}]: first $f, three frames $t, worst $w" >&2
	done
done
median() { echo $@ | tr ' ' '\n' | grep -v '^$' | sort -n | awk '{a[NR]=$1} END {print NR ? a[int((NR+1)/2)] : "-"}'; }
for v in "$@"; do
	printf "%-55s first %8s ms   three frames %8s ms   worst %7s ms   (median of %d)\n" "[${v:-as is}]" \
		$(median ${first[k:$v]}) $(median ${total[k:$v]}) $(median ${worst[k:$v]}) $runs
done
