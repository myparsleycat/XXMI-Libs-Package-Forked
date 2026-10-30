import struct
import sys
from pathlib import Path


data = bytearray(65536)
for offset, colour in [(256, (1, 0, 0, 1)), (512, (0, 1, 0, 1))]:
    constants = [0.0] * 32
    for index in (0, 5, 10, 15):
        constants[index] = 1.0
    constants[24:28] = colour
    struct.pack_into('<32f', data, offset, *constants)
Path(sys.argv[1]).write_bytes(data)
