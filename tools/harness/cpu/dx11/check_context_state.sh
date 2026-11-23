#!/bin/bash
# Run after build.bat. Optional argument: the d3d11.dll to test.
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
common="$here/../../common"
run=$(mktemp -d "$here/bin/context-state-XXXXXX")
status=0
for mode in wrapped hooked; do
	dir="$run/$mode"
	"$common/install_dll.sh" "$dir" "$@"
	settings=(Loader.target=context_state.exe Logging.log_level=info System.allow_platform_update=1 System.config_initialization_delay=-1)
	if [ "$mode" = hooked ]; then
		settings+=(System.hook=immediate_context)
	fi
	python "$common/make_ini.py" "$dir/d3dx.ini" "${settings[@]}"
	cp "$here/bin/context_state.exe" "$dir/"
	"$dir/context_state.exe" "$(cygpath -aw "$dir")" "$mode" || status=$?
done
exit "$status"
