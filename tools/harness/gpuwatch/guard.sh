#!/bin/bash
# guard.sh <run folder> [d3d11.dll to test instead of x64/Release]: a BC7 texture
# in a t slot the shader declares as a structured buffer, bound by the ini, read
# from an index past the end of the texture. Without the guard in
# HackerContext::GuardShaderResources this removes the device (the screen blanks
# for a moment). Exit code 0: every case behaved as the guard should.
here="$(cd "$(dirname "$0")" && pwd)"
dir=$1; dll=$2
bash "$here/../common/install_dll.sh" "$dir" || exit 1
if [ -n "$dll" ]; then cp "$dll" "$dir/d3d11.dll" || exit 1; fi
cp "$here/bin/hang.exe" "$dir/" || exit 1
python -I - "$dir/texture.dds" <<'EOF'
import struct, sys
header = struct.pack('<4s7I44x', b'DDS ', 124, 0x1007, 1024, 1024, 0, 0, 1)
header += struct.pack('<2I4s5I', 32, 4, b'DX10', 0, 0, 0, 0, 0)
header += struct.pack('<5I', 0x1000, 0, 0, 0, 0)
header += struct.pack('<5I', 98, 3, 0, 1, 0)
open(sys.argv[1], 'wb').write(header + bytes(1024 * 1024))
EOF
cat > "$dir/custom_vs.hlsl" <<'EOF'
float4 main(uint id : SV_VertexID) : SV_Position { return float4((id >> 1) * 4.0 - 1, (id & 1) * 4.0 - 1, 0, 1); }
EOF
cat > "$dir/custom_ps.hlsl" <<'EOF'
StructuredBuffer<float4> b : register(t3);
float4 main(float4 p : SV_Position) : SV_Target { return b[1048576]; }
EOF

status=0
# run <name> <expected: fine or removed> <expected Unbound lines> <[Present] lines>
run() {
	local name=$1 expected=$2 unbound=$3 present=$4
	cat > "$dir/d3dx.ini" <<EOF
[Logging]
log_level = info
show_warnings = 0
[Hunting]
hunting = 0
[Rendering]
override_directory = ShaderFixes
cache_directory = ShaderCache
[ResourceTexture]
filename = texture.dds
[ResourceBuffer]
type = StructuredBuffer
stride = 16
array = 16
[CustomShaderRead]
vs = custom_vs.hlsl
ps = custom_ps.hlsl
ps-t3 = ResourceTexture
draw = 3, 0
ps-t3 = null
[CustomShaderReadBuffer]
vs = custom_vs.hlsl
ps = custom_ps.hlsl
ps-t3 = ResourceBuffer
draw = 3, 0
ps-t3 = null
[Present]
$present
EOF
	local output code removed lines result=ok
	rm -f "$dir/d3d11_log.txt"
	output=$(cd "$dir" && timeout 60 ./hang.exe 1 200 0 1048576 mod 2>&1)
	code=$?
	if echo "$output" | grep -q "Present returned"; then
		removed=removed
	elif [ $code = 0 ] && echo "$output" | grep -q "ms per frame"; then
		removed=fine
	else
		removed="no result, exit code $code"
	fi
	lines=$(grep -a -c "^Unbound the texture" "$dir/d3d11_log.txt" 2>/dev/null)
	[ -n "$lines" ] || lines="no log"
	if [ "$removed" != "$expected" ] || [ "$lines" != "$unbound" ]; then
		result=FAILED
		status=1
	fi
	echo "$result $name: $removed, $lines Unbound lines (expected $expected, $unbound)"
}

run "ini binds a texture over the game's structured t1" fine 1 "ps-t1 = ResourceTexture"
run "ini binds a buffer in the game's structured t1" fine 0 "ps-t1 = ResourceBuffer"
run "ini binds a texture in a slot the shader does not declare" fine 0 "ps-t2 = ResourceTexture"
run "custom shader with a texture over its structured t3" fine 1 "run = CustomShaderRead"
run "custom shader with a buffer in its structured t3" fine 0 "run = CustomShaderReadBuffer"

# The same pixel shader with a ShaderFixes replacement, whose declarations count
mkdir -p "$dir/ShaderFixes"
sed -i 's/^log_level = info/log_level = debug/' "$dir/d3dx.ini"
(cd "$dir" && timeout 60 ./hang.exe 1 2 0 1048576 mod >/dev/null 2>&1)
hash=$(grep -a "ps: handle" "$dir/d3d11_log.txt" | sed -n 's/.*hash = \([0-9a-f]*\).*/\1/p' | head -1)
if [ -z "$hash" ]; then
	echo "FAILED: no hash of the pixel shader in the log"
	exit 1
fi
replacement="$dir/ShaderFixes/$hash-ps_replace.txt"

cat > "$replacement" <<'EOF'
Texture2D t : register(t1);
float4 main(float4 p : SV_Position) : SV_Target { return t.Load(int3(0, 0, 0)); }
EOF
run "replacement declares as a texture the t1 the game's shader has structured" fine 0 "ps-t1 = ResourceTexture"

cat > "$replacement" <<'EOF'
StructuredBuffer<float4> b : register(t2);
float4 main(float4 p : SV_Position) : SV_Target { return b[1048576]; }
EOF
run "replacement declares a structured t2 the game's shader does not have" fine 1 "ps-t2 = ResourceTexture"
rm -f "$replacement"
exit $status
