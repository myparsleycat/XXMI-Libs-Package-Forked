# make_mip_dds.py <out.dds>: a 2048x2048 BC7_UNORM texture without mip levels
# whose texels alternate black and white, so that each generated level is
# grey. Every block is the same mode 6 block, which holds the pattern exactly.
import struct
import sys

SIZE = 2048
DXGI_FORMAT_BC7_UNORM = 98


def mode6_checker_block():
    fields = [(0b1000000, 7)]
    fields += [(0, 7), (127, 7)] * 3   # R, G, B: endpoint 0 black, 1 white
    fields += [(127, 7), (127, 7)]     # A
    fields += [(0, 1), (1, 1)]         # p-bits
    for i in range(16):
        x, y = i % 4, i // 4
        fields.append((15 if (x + y) & 1 else 0, 3 if i == 0 else 4))
    value, at = 0, 0
    for bits, width in fields:
        value |= bits << at
        at += width
    assert at == 128
    return value.to_bytes(16, 'little')


header = struct.pack('<4sIIIIIII44s', b'DDS ', 124, 0x1 | 0x2 | 0x4 | 0x1000 | 0x80000, SIZE, SIZE,
                     SIZE * SIZE, 0, 1, b'\0' * 44)
header += struct.pack('<II4sIIIII', 32, 0x4, b'DX10', 0, 0, 0, 0, 0)
header += struct.pack('<IIIII', 0x1000, 0, 0, 0, 0)
header += struct.pack('<IIIII', DXGI_FORMAT_BC7_UNORM, 3, 0, 1, 0)

with open(sys.argv[1], 'wb') as out:
    out.write(header)
    out.write(mode6_checker_block() * ((SIZE // 4) ** 2))
