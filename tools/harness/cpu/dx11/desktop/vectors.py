# vectors.py <vectors.exe> <vector folder>: runs vectors.exe and compares the
# message it decoded from each .bin with the .json next to it. Exits with 1 on
# any difference or when vectors.exe fails.
import json
import os
import subprocess
import sys


def main():
    exe, folder = sys.argv[1], sys.argv[2]
    run = subprocess.run([exe, folder], capture_output=True, text=True, encoding='utf-8')
    sys.stderr.write(run.stderr)
    failures = 0
    seen = set()
    for line in run.stdout.splitlines():
        if not line.startswith('{'):
            print(line)
            continue
        got = json.loads(line)
        name = got.pop('file')
        seen.add(name)
        with open(os.path.join(folder, name[:-4] + '.json'), encoding='utf-8') as f:
            want = json.load(f)
        if got != want:
            print('FAIL %s: decodes to %s' % (name, json.dumps(got, ensure_ascii=False)))
            failures += 1
        else:
            print('PASS %s' % name)
    for name in sorted(os.listdir(folder)):
        if name.endswith('.json') and name[:-5] + '.bin' not in seen:
            print('FAIL %s: no result for its .bin' % name)
            failures += 1
    if run.returncode != 0 or failures:
        sys.exit(1)


main()
