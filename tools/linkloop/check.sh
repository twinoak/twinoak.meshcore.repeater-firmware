#!/usr/bin/env bash
# Builds tools/linkloop (the firmware's own box-link session and clock keeper behind a pipe) and
# drives tools/linkcli.py against it: hello, ping, status, identity, time, time set and reboot,
# with no board and no serial port, plus the timed worst-case pass.
#
# It needs a host C++ compiler. On Windows that is the container tasks/RUNBOOK.md section 2 names,
# from the repository root:
#
#   docker run --rm -v "<repo>:/w" -w /w python:3.11-slim bash tools/linkloop/check.sh
#
# On a machine with g++ and python3 it is just `bash tools/linkloop/check.sh`. Everything it
# writes goes to out/linkloop/, which is git-ignored (tasks/README.md: leave the evidence).
set -eu

out=out/linkloop
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
  tools/linkloop/linkloop.cpp src/link/codec/*.cpp src/link/session/*.cpp \
  src/clock/ClockKeeper.cpp
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

# Every linkcli invocation starts this program afresh, so each one is a *boot* of the board - and
# the clock keeper's file is a real file, so the sequence below is a node being told the time,
# reset, and told again. That is the whole of section 5.4 watched from a laptop.
rm -f "$out/clock"

echo
echo "== time, on a node that has never been told it: upstream's 2024 default, state 0"
$cli time

echo
echo "== time set (this laptop's clock, source 3 - a person at a console): state 2"
$cli time set

echo
echo "== time, after a reset: state 1 (restored), the saved time plus the save interval, and"
echo "   STATUS holding the adverts back (flag bit 3) because nothing has set the clock yet"
$cli time
$cli status

echo
echo "== time set, backwards, on that restored clock: accepted, because nothing carrying a"
echo "   timestamp has left this node since it booted (5.4)"
$cli time set

echo
echo "== the same, on a node that has already put a timestamp on the air: refused, ERR_RANGE"
if python3 tools/linkcli.py --pipe "$out/linkloop --emitted" time set 1767225601; then
  echo "that should have been refused" >&2
  exit 1
fi
echo "refused, as it should be"

echo
echo "== and with --force, a person's instruction, the same request is accepted"
python3 tools/linkcli.py --pipe "$out/linkloop --emitted" time set --force 1767225601
$cli time

echo
echo "== reboot 0"
$cli reboot 0 --delay-ms 10

echo
echo "== one pass of the link, timed (the worst case this task can produce)"
"$out/linkloop" --bench 20000

echo
echo "== all of it passed"
