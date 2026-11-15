"""Check that changing the injected sampler changes only the sampled red channel."""
import sys

from check_dxil_iniparams import pixels

shape, first = pixels(sys.argv[1])
other_shape, second = pixels(sys.argv[2])
if shape != other_shape:
    raise SystemExit("image sizes differ")
changed = 0
for i in range(0, len(first), 4):
    if first[i:i + 2] != second[i:i + 2]:
        raise SystemExit("an unrelated channel changed")
    changed += first[i + 2] != second[i + 2]
if not changed:
    raise SystemExit("changing the sampler did not affect the output")
print(f"{changed} pixels changed with the sampler")
