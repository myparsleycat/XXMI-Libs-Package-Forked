# make_mod.py run/dx12_buffers.csv out.ini [first=100] [count=26]: a mod shaped like a character mod
# for the meshes of the harness: each matched by the hash of its positions, drawn anew with buffers
# and textures of the mod's own in place of the game's draw
import csv
import sys

rows = [row for row in csv.DictReader(open(sys.argv[1])) if row['frame'] == '0']
first = int(sys.argv[3]) if len(sys.argv) > 3 else 100
count = int(sys.argv[4]) if len(sys.argv) > 4 else 26

lines = ['; Made by make_mod.py', '']
positions = []
for v in range(24):
    quad, corner = divmod(v, 4)
    size = 0.3 + quad * 0.12
    positions += ['%.3f' % ((1 if corner & 1 else -1) * size), '%.3f' % ((1 if corner & 2 else -1) * size), '0.5']
indices = []
for quad in range(6):
    indices += [str(quad * 4 + i) for i in (0, 1, 2, 2, 1, 3)]

lines += ['[ResourceModPositions]', 'type = Buffer', 'stride = 12', 'format = R32_FLOAT', 'data = ' + ' '.join(positions), '']
lines += ['[ResourceModIndices]', 'type = Buffer', 'format = R16_UINT', 'data = ' + ' '.join(indices), '']
lines += ['[ResourceModDiffuse]', 'filename = test.dds', '']
lines += ['[ResourceModLight]', 'filename = test.dds', '']

for mesh in range(first, first + count):
    row = rows[mesh * 4]
    assert row['bytes'] == '288', row
    lines += ['[TextureOverrideMesh%d]' % mesh, 'hash = ' + row['hash_vb'], 'handling = skip',
              'vb0 = ResourceModPositions', 'ib = ResourceModIndices',
              'ps-t0 = ResourceModDiffuse', 'ps-t1 = ResourceModLight', 'drawindexed = auto', '']

open(sys.argv[2], 'w', newline='\r\n').write('\n'.join(lines))
