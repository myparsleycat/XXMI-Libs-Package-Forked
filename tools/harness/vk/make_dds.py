# make_dds.py <out.dds>: a BC7_UNORM texture of 4096x4096 with all 13 mips
# (22 MB), for test_bc7: a texture as large as those of character mods. Every
# block is a mode 6 block with fixed random endpoints and indices.
import random
import struct
import sys

SIZE = 4096
MIPS = 13
DXGI_FORMAT_BC7_UNORM = 98

rng = random.Random(7)
blocks = 0
size = SIZE
for _ in range(MIPS):
    blocks += max(1, size // 4) ** 2
    size //= 2

header = struct.pack('<4sIIIIIII44s', b'DDS ', 124, 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | 0x80000, SIZE, SIZE,
                     SIZE * SIZE // 4, 0, MIPS, b'\0' * 44)
# DDS_PIXELFORMAT with the DX10 FourCC, then caps
header += struct.pack('<II4sIIIII', 32, 0x4, b'DX10', 0, 0, 0, 0, 0)
header += struct.pack('<IIIII', 0x1000 | 0x400000 | 0x8, 0, 0, 0, 0)
# DDS_HEADER_DXT10: format, 2D, no flags, one array slice
header += struct.pack('<IIIII', DXGI_FORMAT_BC7_UNORM, 3, 0, 1, 0)

with open(sys.argv[1], 'wb') as out:
    out.write(header)
    # A few thousand distinct blocks repeated keep this quick
    distinct = [bytes([0x40]) + rng.getrandbits(120).to_bytes(15, 'little') for _ in range(4096)]
    chunk = b''.join(distinct)
    left = blocks * 16
    while left:
        n = min(left, len(chunk))
        out.write(chunk[:n])
        left -= n
