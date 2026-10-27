# profdiff.py <profile of appearance frames> <profile of steady frames> [section]
# What the first frames of objects cost over as many steady frames, by
# function: samples of each section turned into ms (the profiler takes ~8000
# a second) and the steady ones subtracted. section: "function" (default),
# "inclusive" or "outside".
import re
import sys

HEADERS = {
    'function': '--- by function',
    'inclusive': '--- inclusive, by function of d3d11.dll',
    'outside': '--- time outside the DLL',
}


def read(path, section):
    text = open(path, encoding='utf-8', errors='replace').read().splitlines()
    samples = 0
    for line in text:
        m = re.match(r'^(\d+) samples$', line)
        if m:
            samples = int(m.group(1))
    values = {}
    inside = False
    for line in text:
        if line.startswith('---'):
            inside = line.startswith(HEADERS[section])
            continue
        m = re.match(r'^\s*([\d.]+)%\s+(.*)$', line)
        if inside and m:
            values[m.group(2).strip()] = float(m.group(1)) / 100 * samples / 8.0
    return values, samples


section = sys.argv[3] if len(sys.argv) > 3 else 'function'
a, na = read(sys.argv[1], section)
b, nb = read(sys.argv[2], section)
print('appearance frames %.0f ms sampled, steady %.0f ms; difference %.0f ms' % (na / 8.0, nb / 8.0, (na - nb) / 8.0))
rows = sorted(((a.get(k, 0) - b.get(k, 0), a.get(k, 0), b.get(k, 0), k) for k in set(a) | set(b)), reverse=True)
for diff, x, y, k in rows[:int(sys.argv[4]) if len(sys.argv) > 4 else 40]:
    print('%8.0f ms  (%7.0f - %7.0f)  %s' % (diff, x, y, k[:110]))
