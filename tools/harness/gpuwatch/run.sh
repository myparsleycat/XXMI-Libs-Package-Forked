#!/bin/bash
# run.sh <run folder> <gpu_watch> <hunting> [hang.exe arguments]: runs hang.exe
# through x64/Release/d3d11.dll and prints d3d11_gpu_watch.txt. A hang frame in
# the arguments makes the driver reset the GPU, which blanks the screen for a moment.
# DEBUG_LAYER=1 turns on gpu_watch_debug_layer; the mod section binds a t0 of the
# wrong stride, which the debug layer has to report.
# AFTERMATH=1 turns on gpu_watch_aftermath; the library has to be in the run folder.
here="$(cd "$(dirname "$0")" && pwd)"
dir=$1; watch=$2; hunting=$3; shift 3
bash "$here/../common/install_dll.sh" "$dir" || exit 1
cp "$here/bin/hang.exe" "$dir/" || exit 1
rm -f "$dir/d3d11_gpu_watch.txt"
cat > "$dir/mod.hlsl" <<'EOF'
StructuredBuffer<float4> t : register(t0);
float4 main(uint id : SV_VertexID) : SV_Position { return float4(id & 1, id >> 1, 0, 1) * 0.01 + t[0]; }
EOF
cat > "$dir/mod_ps.hlsl" <<'EOF'
float4 main() : SV_Target { return 0.5; }
EOF
cat > "$dir/d3dx.ini" <<EOF
[Logging]
log_level = info
show_warnings = 0
gpu_watch = $watch
gpu_watch_debug_layer = ${DEBUG_LAYER:-0}
gpu_watch_aftermath = ${AFTERMATH:-0}
[Hunting]
hunting = $hunting
[ResourceWrongStride]
type = StructuredBuffer
stride = 32
array = 4
[CustomShaderWatchTest]
vs = mod.hlsl
ps = mod_ps.hlsl
vs-t0 = ResourceWrongStride
draw = 3, 0
vs-t0 = null
[Present]
run = CustomShaderWatchTest
EOF
cd "$dir" || exit 1
./hang.exe "$@"
echo "exit code $?"
[ -f d3d11_gpu_watch.txt ] && cat d3d11_gpu_watch.txt
exit 0
