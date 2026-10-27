# make_mod.py <mods dir> <mode>: a character mod that replaces a skinned mesh, for skin.exe (check.sh
# lists the skin.exe arguments each mode is for and what it has to show)
# mode 0: vb0 = the mod's position buffer; mode 1: vb0 = what its compute shader made of a copy of it;
# mode 2: mode 0 without handling = skip on its skin section, as some hair sections have it;
# mode 3: mode 2 that binds the game's vb0 back after its draw; mode 4: the same with a copy of it;
# mode 5: an older mod that goes by ...Position (a hash D3D12 never gives) and ...VertexLimitRaise, no ...Blend
# mode 6: mode 5 plus another such mod for a different mesh of as many vertices: neither may be taken
# mode 7: mode 5, for skin.exe second: the second mesh has as many vertices as the mod's and no mod
# mode 8: mode 5, for skin.exe drawfirst, with draws that check vb0 and vb2 only (try.sh swaps harness.ini)
# mode 9: mode 5, for skin.exe unhashed second second_at=10: weights nothing has a hash of
# mode 10: mode 5, for skin.exe reuse_at=10: the buffer skinned to had another mesh of as many vertices first
# mode 11: mode 5, for skin.exe replace_at=10: a mesh of another size takes over the address a mesh of the mod's size was noted at
# mode 12: mode 0 that copies so0 after its draw and binds the copy as vb0 of its ...IB draw: so0 is as large as the mod's
#          900 vertices, more than the game's buffer of 300 has
import os, struct, sys
out, mode = sys.argv[1], int(sys.argv[2])
os.makedirs(out, exist_ok=True)
GX, GY = 30, 30
pos, blend, ib = bytearray(), bytearray(), bytearray()
for y in range(GY):
    for x in range(GX):
        pos += struct.pack('<10f', 0.1 + 0.4 * x / (GX - 1), -0.2 + 0.4 * y / (GY - 1), 0.5, 0, 0, 1, 1, 0, 0, 1)
        blend += struct.pack('<4f4I', 1, 0, 0, 0, 0, 0, 0, 0)
count = 0
for y in range(GY - 1):
    for x in range(GX - 1):
        a = y * GX + x
        ib += struct.pack('<6I', a, a + 1, a + GX, a + 1, a + GX + 1, a + GX)
        count += 6
open(os.path.join(out, 'pos.buf'), 'wb').write(pos)
open(os.path.join(out, 'blend.buf'), 'wb').write(blend)
open(os.path.join(out, 'ib.buf'), 'wb').write(ib)
V = GX * GY
open(os.path.join(out, 'anim.hlsl'), 'w').write('''struct Vertex { float3 position; float3 normal; float4 tangent; };
StructuredBuffer<Vertex> Live : register(t50);
RWStructuredBuffer<Vertex> Output : register(u5);
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
	uint i = id.x;
	if (i >= %d) return;
	uint count, stride;
	Live.GetDimensions(count, stride);
	Vertex v = Output[i];
	v.position.x += 0.4;
	// Off the row the harness looks at when the game's vertices did not come through
	if (count != 300)
		v.position.y += 0.6;
	else if (stride != 40)
		v.position.y -= 0.6;
	else if (Live[0].position.x > -0.79 || Live[0].position.x < -0.81)
		v.position.y += 0.3;
	Output[i] = v;
}
''' % V)
open(os.path.join(out, 'skin.ini'), 'w').write('''[TextureOverrideSkinBlend]
hash = 69bfccb7
vb2 = ResourceSkinBlend
if DRAW_TYPE == 1
%s%s
	draw = %d, 0
%sendif

[TextureOverrideSkinIB]
hash = 52a140fd
handling = skip
ib = ResourceSkinIB
drawindexed = %d, 0, 0

[CustomShaderAnim]
cs = anim.hlsl
ResourceLive = copy vb0
cs-t50 = ref ResourceLive
cs-u5 = copy ResourceSkinPosition
ResourceAnimated = ref cs-u5
Dispatch = %d, 1, 1
cs-u5 = null
cs-t50 = null

[ResourceLive]
type = StructuredBuffer
stride = 40

[ResourceAnimated]

[ResourceGameVb0]

[ResourceSkinPosition]
type = Buffer
stride = 40
filename = pos.buf

[ResourceSkinBlend]
type = Buffer
stride = 32
filename = blend.buf

[ResourceSkinIB]
type = Buffer
format = DXGI_FORMAT_R32_UINT
filename = ib.buf
''' % ('' if mode in (2, 3, 4) else '\thandling = skip\n',
      '\tResourceGameVb0 = ref vb0\n\tvb0 = ResourceSkinPosition' if mode == 3 else
      '\tResourceGameVb0 = copy vb0\n\tvb0 = ResourceSkinPosition' if mode == 4 else
      '\tvb0 = ResourceSkinPosition' if mode in (0, 2, 12) else '\trun = CustomShaderAnim\n\tvb0 = ResourceAnimated',
      V, '\tvb0 = ResourceGameVb0\n' if mode in (3, 4) else '\tResourceSkinCopy = copy so0\n' if mode == 12 else '',
      count, (V + 63) // 64))

if mode == 12:
    ini = open(os.path.join(out, 'skin.ini')).read()
    ini = ini.replace('ib = ResourceSkinIB\n', 'ib = ResourceSkinIB\nvb0 = ResourceSkinCopy\n')
    open(os.path.join(out, 'skin.ini'), 'w').write(ini + '''
[ResourceSkinCopy]
type = Buffer
stride = 40
''')

if mode in (5, 6, 7, 8, 9, 10, 11):
    def crc32c(b):
        c = 0xffffffff
        for x in b:
            c ^= x
            for _ in range(8):
                c = (c >> 1) ^ 0x82F63B78 if c & 1 else c >> 1
        return c ^ 0xffffffff
    GAME_VERTICES = 20 * 15
    vlr = crc32c(struct.pack('<6I', GAME_VERTICES * 40, 0, 0x19, 0, 0, 0))
    ini = open(os.path.join(out, 'skin.ini')).read()
    head, rest = ini.split('[TextureOverrideSkinIB]', 1)
    open(os.path.join(out, 'skin.ini'), 'w').write('''[TextureOverrideSkinPosition]
hash = 0badf00d
handling = skip
vb0 = ResourceSkinPosition
vb2 = ResourceSkinBlend
draw = %d, 0

[TextureOverrideSkinTexcoord]
hash = 0badf00e

[TextureOverrideSkinVertexLimitRaise]
hash = %08x

[TextureOverrideSkinIB]''' % (V, vlr) + rest)

if mode == 6:
    open(os.path.join(out, 'other.ini'), 'w').write('''[TextureOverrideOtherPosition]
hash = 0badf0ff
handling = skip

[TextureOverrideOtherVertexLimitRaise]
hash = %08x
''' % vlr)
