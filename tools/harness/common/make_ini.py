# make_ini.py <out d3dx.ini> [Section.key=value ...]
# Writes the d3dx.ini the package ships (Dependencies/d3dx.ini) with the given
# settings changed: a key already in its section, commented out or not, is
# replaced in place, any other is added right below the section's header.
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
STOCK = os.path.join(HERE, '..', '..', '..', 'Dependencies', 'd3dx.ini')


def main():
    if len(sys.argv) < 2:
        raise SystemExit('make_ini.py <out d3dx.ini> [Section.key=value ...]')
    lines = open(STOCK, encoding='utf-8', newline='').read().splitlines()
    for setting in sys.argv[2:]:
        m = re.match(r'^([^.=]+)\.([^=]+)=(.*)$', setting)
        if not m:
            raise SystemExit('not Section.key=value: %s' % setting)
        section, key, value = m.group(1).lower(), m.group(2).strip(), m.group(3).strip()
        current = None
        header = None
        done = False
        for i, line in enumerate(lines):
            s = re.match(r'^\s*\[([^\]]+)\]', line)
            if s:
                current = s.group(1).lower()
                if current == section and header is None:
                    header = i
                continue
            if current == section and re.match(r'^\s*;?\s*%s\s*=' % re.escape(key), line, re.I):
                lines[i] = '%s = %s' % (key, value)
                done = True
                break
        if not done:
            if header is None:
                raise SystemExit('no [%s] section in %s' % (section, STOCK))
            lines.insert(header + 1, '%s = %s' % (key, value))
    open(sys.argv[1], 'w', encoding='utf-8', newline='\r\n').write('\n'.join(lines) + '\n')


main()
