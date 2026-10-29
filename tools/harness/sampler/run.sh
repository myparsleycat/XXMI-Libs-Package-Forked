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
./bin/harness.exe "$(cygpath -aw "$dir")" "${2:-2}"
status=$?
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
exit $status
