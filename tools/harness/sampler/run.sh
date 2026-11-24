#!/bin/bash
# run.sh [new|old|stock|<d3d11.dll>] [frames]: a fresh run folder with that DLL
# and case/, then the harness, which checks the last frame (exit 2 on a
# failed check). new is x64/Release/d3d11.dll (the default); old and stock
# are a package DLL from before sampler slots and the upstream one, under
# XXMI_PACKAGES (the packages folder of the launcher), to compare with.
here="$(cd "$(dirname "$0")" && pwd)"
cd "$here" || exit 1
[ -x bin/harness.exe ] || { echo "no bin/harness.exe: run build.bat first"; exit 1; }
which=${1:-new}
case "$which" in
	new) dll=../../../x64/Release/d3d11.dll ;;
	old|stock)
		[ -n "$XXMI_PACKAGES" ] || { echo "set XXMI_PACKAGES to the launcher's packages folder"; exit 1; }
		dll=$XXMI_PACKAGES/xxmi-libs-myparsleycat/1.2.2-nhd.12/d3d11.dll
		[ $which = stock ] && dll=$XXMI_PACKAGES/xxmi-libs/1.2.2/d3d11.dll ;;
	*) dll=$which; which=other ;;
esac
[ -f "$dll" ] || { echo "no DLL at $dll"; exit 1; }
dir=run/$which
rm -rf "$dir"
mkdir -p "$dir"
cp case/* "$dir"/ || exit 1
cp "$dll" "$dir"/d3d11.dll || exit 1
python make_mip_dds.py "$dir/mip_checker.dds" || exit 1
current=
[ $which != stock ] && [ $which != old ] && current=1
./bin/harness.exe "$(cygpath -aw "$dir")" "${2:-2}" ${current:+wait_mips} | tee "$dir/out1.txt"
status=${PIPESTATUS[0]}
# Every line of [CommandListBad] and every bad key of [ResourceBadKeys] has a
# warning of its own (the configuration is loaded twice, so each comes twice)
warned() {
	grep -B1 "^ - \[$1\]" "$dir/d3d11_log.txt" 2>/dev/null | grep -v "^ - \|^--" | sort -u | wc -l
}
if [ $which != stock ] && [ $which != old ]; then
	for want in commandlistbad:9 ResourceBadKeys:5; do
		section=${want%:*}
		got=$(warned $section)
		if [ "$got" = "${want#*:}" ]; then
			echo "PASS $got warnings for [$section]"
		else
			echo "FAIL $got warnings for [$section], expected ${want#*:}"
			status=2
		fi
	done
fi

# Generated mip levels of mip_checker.dds: none on the first load, which
# queues them; used from the next; a damaged cache file is deleted and made
# again; mod_texture_max_size leaves out the levels above it. The DLL loads
# the configuration twice, and the second load may already find the levels,
# so the first load of a run is checked in the log.
first_load() {
	local got
	got=$(grep -m1 "mip_checker.dds as DDS" "$dir/$1")
	if echo "$got" | grep -q "$2"; then
		echo "PASS $3"
	else
		echo "FAIL $3: $got"
		status=2
	fi
}
mips_check() {
	local got
	got=$(grep "^  mips:" "$dir/$1" | tail -1)
	if echo "$got" | grep -q "$2"; then
		echo "PASS $3"
	else
		echo "FAIL $3:$got"
		status=2
	fi
}
again() {
	./bin/harness.exe "$(cygpath -aw "$dir")" 2 $2 > "$dir/$1"
	cp "$dir/d3d11_log.txt" "$dir/log_$1"
	grep "^  mips:\|^mips cache" "$dir/$1" | tail -2
}
if [ $which != stock ] && [ $which != old ]; then
	cp "$dir/d3d11_log.txt" "$dir/log_out1.txt"
	first_load log_out1.txt "mip_checker.dds as DDS, " "first load: mip_checker.dds as stored"
	grep -q "^Generated .*mip_checker.dds" "$dir/log_out1.txt" || { echo "FAIL no mip levels generated after the first load"; status=2; }
	again out2.txt
	mips_check out2.txt " 12 levels, width 2048, level 3 texel 0.50[0-9], level 0 texel 0.000" "next run: 12 levels, level 3 grey"
	cached=$(ls "$dir"/cache/mips/*_mips.bin 2>/dev/null | head -1)
	if [ -n "$cached" ]; then
		# Its header still matches; the levels no longer have their crc
		python -I -c "import sys; f = open(sys.argv[1], 'r+b'); f.seek(-1, 2); b = f.read(1)[0]; f.seek(-1, 2); f.write(bytes([b ^ 0xff]))" "$cached"
		# Otherwise the prefetch thread makes it again before the first load
		sed -i 's/^\[Rendering\]$/[Rendering]\nprefetch_resource_files = 0/' "$dir/d3dx.ini"
		again out3.txt wait_mips
		first_load log_out3.txt "mip_checker.dds as DDS, " "damaged cache file: loaded as stored"
		grep -q "are damaged and are deleted" "$dir/log_out3.txt" || { echo "FAIL damaged cache file not reported"; status=2; }
		grep -q "^Generated .*mip_checker.dds" "$dir/log_out3.txt" || { echo "FAIL damaged cache file not made again"; status=2; }
	fi
	sed -i 's/^\[Rendering\]$/[Rendering]\nmod_texture_max_size = 1024/' "$dir/d3dx.ini"
	again out4.txt
	mips_check out4.txt " 11 levels, width 1024, level 3 texel 0.50[0-9]" "mod_texture_max_size = 1024: 11 levels from 1024"
fi
exit $status
