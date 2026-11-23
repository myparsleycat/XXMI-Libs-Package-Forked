#!/bin/bash
# bench.sh [runs] [harness args]: the median recording and presenting times a
# frame of the harness without the DLL, with the one in ref and with the one
# in run, the three taken in turn for each of some runs, and run/ref
cd "$(dirname "$0")"
runs=${1:-3}
shift
harness_args=("$@")
. ../common/bench.inc.sh
bench_run() {
	case $1 in
	nodll) (cd run && ./harness.exe frames=4300 nodll "${harness_args[@]}") ;;
	*) (cd $1 && ./harness.exe frames=4300 "${harness_args[@]}") ;;
	esac
}
bench_value() { sed -n 's/.*recording and executing \([0-9.]*\) ms, presenting.*/\1/p'; }
bench_extra() { sed -n 's/.*recording and executing [0-9.]* ms, presenting \([0-9.]*\) ms.*/\1/p'; }
BENCH_EXTRA_NAME="ms presenting"
bench_rounds $runs nodll ref run
