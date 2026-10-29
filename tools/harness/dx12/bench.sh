#!/bin/bash
# bench.sh [runs] [harness args]: the recording time a frame of the harness
# without the DLL, with the one in ref and with the one in run, each the median
# of some runs
cd "$(dirname "$0")"
runs=${1:-3}
shift
for dir in nodll ref run; do
	folder=$dir
	extra=
	if [ $dir = nodll ]; then
		folder=run
		extra=nodll
	fi
	values=$(for i in $(seq $runs); do (cd $folder && ./harness.exe frames=4300 $extra "$@") | sed -n 's/.*recording and executing \([0-9.]*\) ms, presenting \([0-9.]*\).*/\1 \2/p'; done)
	echo "$dir: record $(echo "$values" | awk '{print $1}' | sort -n | sed -n "$(( (runs + 1) / 2 ))p") ms, present $(echo "$values" | awk '{print $2}' | sort -n | sed -n "$(( (runs + 1) / 2 ))p") ms   (all: $(echo $values))"
done
