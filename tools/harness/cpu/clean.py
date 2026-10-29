# clean.py <importer|--all>
# Removes the run folders setup.py made. The junctions to the real Core, Mods
# and ShaderFixes folders are unlinked first and nothing is ever followed
# through one, so the deployed files stay as they are.
import os
import re
import shutil
import sys

from paths import RUNS


def unlink_junctions(top):
    for base, dirs, files in os.walk(top, followlinks=False):
        for d in list(dirs):
            path = os.path.join(base, d)
            if os.path.islink(path) or os.path.isjunction(path):
                os.rmdir(path)
                dirs.remove(d)


def main():
    if len(sys.argv) != 2:
        raise SystemExit('clean.py <importer|--all>')
    if sys.argv[1] == '--all':
        top = RUNS
    else:
        if not re.fullmatch(r'[A-Za-z0-9_-]+', sys.argv[1]):
            raise SystemExit('not an importer name: %s' % sys.argv[1])
        top = os.path.join(RUNS, sys.argv[1])
        if os.path.dirname(os.path.realpath(top)) != os.path.realpath(RUNS):
            raise SystemExit('%s is not a folder of %s, nothing deleted' % (top, RUNS))
    if not os.path.isdir(top):
        return
    unlink_junctions(top)
    for base, dirs, files in os.walk(top, followlinks=False):
        for d in dirs:
            path = os.path.join(base, d)
            if os.path.islink(path) or os.path.isjunction(path):
                raise SystemExit('a junction is left at %s, nothing deleted' % path)
    shutil.rmtree(top)
    print('removed %s' % top)


main()
