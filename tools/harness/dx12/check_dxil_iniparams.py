"""Check that the IniParams patch raises only the red output channel."""
import struct
import sys


def pixels(path):
    data = open(path, "rb").read()
    if data[:2] != b"BM":
        raise ValueError("not a BMP file")
    offset = struct.unpack_from("<I", data, 10)[0]
    width, height = struct.unpack_from("<ii", data, 18)
    bits = struct.unpack_from("<H", data, 28)[0]
    compression = struct.unpack_from("<I", data, 30)[0]
    if width <= 0 or height == 0 or bits != 32 or compression != 0:
        raise ValueError("expected an uncompressed 32-bit BMP")
    size = width * abs(height) * 4
    if len(data) < offset + size:
        raise ValueError("truncated BMP")
    return (width, height), data[offset:offset + size]


def main():
    if len(sys.argv) != 3:
        raise ValueError("expected patched and unpatched images")
    shape, patched = pixels(sys.argv[1])
    original_shape, original = pixels(sys.argv[2])
    if shape != original_shape:
        raise ValueError("image sizes differ")
    changed = 0
    for i in range(0, len(patched), 4):
        b, g, r = patched[i:i + 3]
        old_b, old_g, old_r = original[i:i + 3]
        if b != old_b or g != old_g or r < old_r:
            raise ValueError("unexpected channel changed")
        changed += r > old_r
    if not changed:
        raise ValueError("IniParams did not affect the output")
    print(f"{changed} pixels read IniParams")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, struct.error) as error:
        print(error, file=sys.stderr)
        sys.exit(1)
