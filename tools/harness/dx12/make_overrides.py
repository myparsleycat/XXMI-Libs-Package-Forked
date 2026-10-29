# make_overrides.py <out ini> [count]: TextureOverride sections whose hashes
# nothing in the harness has (fixed random ones), each setting a variable, for
# the lookups a configuration with many mods installed makes on every draw
import random
import sys

count = int(sys.argv[2]) if len(sys.argv) > 2 else 300
rng = random.Random(1)
lines = ['[Constants]'] + ['global $nothing%d = 0' % i for i in range(7)] + ['']
for i in range(count):
    lines += ['[TextureOverrideNothing%d]' % i, 'hash = %08x' % rng.getrandbits(32), '$nothing%d = 1' % (i % 7), '']
open(sys.argv[1], 'w', newline='\r\n').write('\n'.join(lines))
