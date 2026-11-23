# Sourced by the bench commands. The caller defines bench_run <variant>, which
# runs the harness once for that variant and prints its output, and
# bench_value, which reads the time to compare from that output on stdin;
# only its first line is used.
# It may also define bench_extra, which reads a second number from the output,
# and set BENCH_EXTRA_NAME to its unit and meaning, e.g. "ms presenting".
#
# bench_rounds <rounds> <variant...> runs every variant once per round, in
# turn, so that a slow change of the machine's speed during the bench reaches
# all variants alike. It prints the median of each variant with the share of
# the processors that other processes used in each round and, when there is
# a variant named ref, the median over the rounds of each other variant's
# time divided by ref's time of the same round (not for nodll). BENCH_LABEL
# goes before every line. A run that gives no time is shown as "-" in its
# round and left out of the medians; bench_rounds then returns 1.

bench_median() {
	echo "$@" | tr ' ' '\n' | grep -E '^[0-9.]+$' | sort -g | awk '{ a[NR] = $1 } END { print NR ? a[int((NR + 1) / 2)] : "-" }'
}

bench_rounds() {
	local rounds=$1 r v out value extra load failed=0
	shift
	local -A values extras loads
	for r in $(seq $rounds); do
		for v in "$@"; do
			out=$(bench_run $v)
			value=$(echo "$out" | bench_value | head -1)
			if [ -z "$value" ]; then
				echo "$out" | tail -5 >&2
				echo "${BENCH_LABEL}$v: no time in the output of round $r" >&2
				failed=1
			fi
			values[$v]="${values[$v]} ${value:--}"
			if declare -F bench_extra > /dev/null; then
				extra=$(echo "$out" | bench_extra | head -1)
				extras[$v]="${extras[$v]} ${extra:--}"
			fi
			load=$(echo "$out" | sed -n 's/^other processes: \([0-9]*\)%.*/\1/p')
			loads[$v]="${loads[$v]} ${load:-?}%"
		done
	done
	for v in "$@"; do
		local line="${BENCH_LABEL}$v: $(bench_median ${values[$v]}) ms"
		[ -n "${extras[$v]}" ] && line="$line, $(bench_median ${extras[$v]}) $BENCH_EXTRA_NAME"
		echo "$line   (rounds:${values[$v]}; other processes:${loads[$v]})"
	done
	if [ -n "${values[ref]}" ]; then
		for v in "$@"; do
			[ $v = ref ] || [ $v = nodll ] && continue
			local ratios=$(paste -d' ' <(echo ${values[ref]} | tr ' ' '\n') <(echo ${values[$v]} | tr ' ' '\n') |
				awk '$1 ~ /^[0-9.]+$/ && $2 ~ /^[0-9.]+$/ && $1 > 0 { printf "%.3f\n", $2 / $1; next } { print "-" }')
			echo "${BENCH_LABEL}$v/ref: $(bench_median $ratios)   (rounds: $(echo $ratios))"
		done
	fi
	return $failed
}
