# bmp2png.py in.bmp out.png [scale]: the dumps of harness.exe as something an image viewer opens
import struct
import sys
import zlib

data = open(sys.argv[1], 'rb').read()
offset = struct.unpack_from('<I', data, 10)[0]
width, height = struct.unpack_from('<ii', data, 18)
scale = int(sys.argv[3]) if len(sys.argv) > 3 else 1
rows = []
for y in range(abs(height)):
    row = data[offset + y * width * 4: offset + (y + 1) * width * 4]
    rgb = bytearray()
    for x in range(width):
        b, g, r = row[x * 4], row[x * 4 + 1], row[x * 4 + 2]
        rgb += bytes((r, g, b)) * scale
    for _ in range(scale):
        rows.append(b'\x00' + bytes(rgb))
if height > 0:
    rows.reverse()


def chunk(kind, body):
    return struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body) & 0xffffffff)


png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width * scale, abs(height) * scale, 8, 2, 0, 0, 0))
png += chunk(b'IDAT', zlib.compress(b''.join(rows), 6)) + chunk(b'IEND', b'')
open(sys.argv[2], 'wb').write(png)
