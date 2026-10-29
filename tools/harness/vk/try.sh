#!/bin/bash
# try.sh <name of a case/tests/*.ini without the ending> [arguments of the harness]
# Runs the harness with that for its test.ini and prints what it measured.
# LINES_WANTED=N shows more lines of vk_report.txt (default 4).
cd "$(dirname "$0")/run" || exit 1
ini=$1; shift
cp "../case/tests/$ini.ini" Mods/test.ini || exit 1
./harness.exe "$@" 2>&1 | grep -v "^device\|^format\|^destroying\|^done"
status=${PIPESTATUS[0]}
grep "^Mods, frame\|^  Storage\|^  Of the time\|^  The draws\|^\*\*\*" vk_report.txt | tail -${LINES_WANTED:-4} | sed 's/, [0-9]* buffers of mods copied.*per frame the draws took/; draws took/; s/; [0-9]* shaders linked.*//' | cut -c1-760
cp ../case/tests/test_texture.ini Mods/test.ini
exit $status
