#!/usr/bin/env bash
# Builds tools/linkloop (the firmware's own box-link session behind a pipe) and drives
# tools/linkcli.py against it: hello, ping, status, identity and reboot, with no board and no
# serial port, plus the timed worst-case pass.
#
# It needs a host C++ compiler. On Windows that is the container tasks/RUNBOOK.md section 2 names,
# from the repository root:
#
#   docker run --rm -v "<repo>:/w" -w /w python:3.11-slim bash tools/linkloop/check.sh
#
# On a machine with g++ and python3 it is just `bash tools/linkloop/check.sh`. Everything it
# writes goes to out/p1-03/, which is git-ignored (tasks/README.md: leave the evidence).
set -eu

out=out/p1-03
mkdir -p "$out"

if ! command -v g++ > /dev/null; then
  if command -v apt-get > /dev/null; then
    echo "== installing g++"
    apt-get update -qq && apt-get install -y -qq g++ > /dev/null 2>&1
  else
    echo "no g++ and no apt-get: see the header of this file" >&2
    exit 2
  fi
fi

echo "== build"
g++ -std=c++17 -O2 -Wall -Wextra -Werror -I src -o "$out/linkloop" \
  tools/linkloop/linkloop.cpp src/link/codec/*.cpp src/link/session/*.cpp
echo "built $out/linkloop"

cli="python3 tools/linkcli.py --pipe $out/linkloop"

echo
echo "== linkcli --help (no pyserial needed)"
python3 tools/linkcli.py --help > /dev/null
echo "help ok"

echo
echo "== hello (frames shown, and the EV_BOOT the session sent on its own)"
$cli -v hello

echo
echo "== ping"
$cli ping --nonce 0xdeadbeef

echo
echo "== status"
$cli status

echo
echo "== identity"
$cli identity

echo
echo "== reboot 0"
$cli reboot 0 --delay-ms 10

echo
echo "== one pass of the link, timed (the worst case this task can produce)"
"$out/linkloop" --bench 20000

echo
echo "== all of it passed"
