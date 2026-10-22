#!/usr/bin/env bash
# Builds tools/linkloop (the firmware's own box-link session, clock keeper, attention machine,
# settings table, power/environment readers, mesh-client job and observation ring behind a pipe)
# and drives tools/linkcli.py against it: hello, ping, status, identity, time, time set, cli,
# power, env, lpp, the job and its path, the five sends, the observations, sleep, an attention,
# the first-boot profile and `provision`, and reboot, with no board and no serial port, plus the
# timed worst-case pass.
#
# **tools/verify.ps1 runs this**, in its native step, with the compiler that step uses - this
# machine's g++ where there is one and the container of tasks/RUNBOOK.md section 2 where there is
# not - so an include of this program's that no longer resolves is a red verification and not a
# surprise for whoever next needs it (phase 3's review; tasks/phase-3/reports/03.md found it
# stale). It needs a host C++ compiler and python3 and nothing else. By hand, from the repository
# root:
#
#   docker run --rm -v "<repo>:/w" -w /w python:3.11-slim bash tools/linkloop/check.sh
#
# On a machine with g++ and python3 it is just `bash tools/linkloop/check.sh`, or
# `tools/verify.ps1 -Only native`. Everything it writes goes to out/linkloop/, which is
# git-ignored (tasks/README.md: leave the evidence).
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

# Two of the firmware's own defines, read out of platformio.ini rather than written here (the
# same habit as tools/verify.ps1, which reads the board environments from that file so that there
# is no second list to forget): the repeater image's band, and that band's duty cycle.
# src/roles/repeater/firstboot/Profile.cpp builds the profile's `set dutycycle` line from the
# second and refuses to compile without either, because the line is the band's and not this
# file's (src/common/bands/Bands.h carries the same number and the two are held against each
# other where the table is).
ini=platformio.ini
insec='$0==s{f=1;next} /^\[/{f=0}'   # awk: the lines of the section named by s, and no more
band=$(awk -v s="[img_repeater_rak4630_single]" "$insec f&&\$1==\"band\"{print \$3;exit}" "$ini")
duty=$(awk -v s="[band_$band]" "$insec f&&\$1==\"duty_cycle\"{print \$3;exit}" "$ini")
test -n "$band" || { echo "platformio.ini: no band for the repeater image" >&2; exit 2; }
test -n "$duty" || { echo "platformio.ini: [band_$band] has no duty_cycle" >&2; exit 2; }
echo "== band $band, duty cycle ${duty}% (platformio.ini)"

echo "== build"
# src/common/power/BoardPower.cpp is the one file of that directory left out: it is the board's half -
# Wire, the Adafruit driver and the file system - and tools/linkloop/ has its own fakes for each.
g++ -std=c++17 -O2 -Wall -Wextra -Werror -I src \
  -D TWINOAK_BAND="\"$band\"" -D TWINOAK_BAND_DUTY_CYCLE="$duty" \
  -o "$out/linkloop" \
  tools/linkloop/linkloop.cpp src/common/link/codec/*.cpp src/common/link/session/*.cpp \
  src/common/link/attention/*.cpp src/roles/repeater/clock/ClockKeeper.cpp src/roles/repeater/app/Settings.cpp \
  src/roles/repeater/firstboot/Profile.cpp \
  src/roles/repeater/meshclient/Job.cpp src/roles/repeater/meshclient/LinkMeshClient.cpp \
  src/roles/repeater/meshclient/LinkObservations.cpp src/roles/repeater/meshclient/ObservationRing.cpp \
  src/roles/repeater/meshclient/Reply.cpp src/roles/repeater/meshclient/SendCeiling.cpp \
  src/common/power/BatteryWatch.cpp src/common/power/Environment.cpp src/common/power/Ina3221.cpp \
  src/common/power/LinkPower.cpp src/common/power/PowerReader.cpp src/common/power/Prefs.cpp \
  src/common/power/Telemetry.cpp
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
echo "== cli: one console command line, with console privileges (5.7). The settings table in"
echo "   these answers is the firmware's own; upstream's interpreter is the one thing a host"
echo "   build cannot have."
$cli cli "get radio"
$cli cli "ver"
$cli cli "sensor list"
$cli cli "sensor get walter.wake"

echo
echo "== cli: a line that is not legal is ERR_MALFORMED and is never run (a tab in it)"
if $cli cli "get$(printf '\t')radio"; then
  echo "a control character should have been refused" >&2
  exit 1
fi
echo "refused, as it should be"

echo
echo "== cli: a command that restarts the node is not owed an answer"
$cli cli "reboot"

echo
echo "== power: the three INA3221 channels, each with its own shunt applied (5.8). The registers"
echo "   are a fake bus's; the arithmetic, the cache and the signs are the firmware's own."
$cli power

echo
echo "== env: the air in the enclosure, below freezing"
$cli env

echo
echo "== lpp: exactly what a mesh telemetry request would be answered with, read back field by"
echo "   field - the channel table is docs/link.md's and never changes"
$cli lpp
echo
echo "   and with base permission alone, which is what a guest gets:"
$cli lpp 1

echo
echo "== power with the B-board unplugged: valid 0, every channel 0, and the node still answers"
python3 tools/linkcli.py --pipe "$out/linkloop --drop-ina" power

echo
echo "== the mesh client's job (6.1): begin a peer and a path, answered OK, and end it. This is"
echo "   the state and the path codec; nothing goes on the air through this program."
$cli job begin a1b2c3d4e5f60718293a4b5c6d7e8f90a1b2c3d4e5f60718293a4b5c6d7e8f90 --path a1b2,7f3a
$cli job end

echo
echo "== a job is per-boot RAM, so --job begins one at startup and this one invocation reads the"
echo "   route back and sees STATUS's job bit (0x0001) set - what a board holds across one boot"
python3 tools/linkcli.py --pipe "$out/linkloop --job" path
python3 tools/linkcli.py --pipe "$out/linkloop --job" status

echo
echo "== the five sends (6.2) through the firmware's own client. The port under this program"
echo "   transmits nothing, so a send with a timeout is answered at once (send 2 = direct along"
echo "   the job's path, tag, sender_ts) and its wait then ends in EV_MESH_REPLY state 2"
echo "   (silence) - the send, the event path and the deadline, watched from a laptop. A real"
echo "   repeater on the bench answers before the timeout (task 02's bench list). The exit code"
echo "   of a send that times out is 1, which is expected here and not a failure of the link, so"
echo "   each is run with '|| true'. The password is read from stdin, never the command line."
echo "example-pw" | python3 tools/linkcli.py --pipe "$out/linkloop --job" login - --timeout-ms 400 || true
python3 tools/linkcli.py --pipe "$out/linkloop --job" status-of-peer --timeout-ms 300 || true
python3 tools/linkcli.py --pipe "$out/linkloop --job" send-cli "get name" --timeout-ms 300 || true

echo
echo "== discovery always floods (send 1), whatever path the job holds; a trace needs no job"
python3 tools/linkcli.py --pipe "$out/linkloop --job" discover --timeout-ms 300 || true
$cli trace 7f3aa1b27f3a46f6 --timeout-ms 300 || true

echo
echo "== refusals: no job is ERR_STATE; three-byte hashes in a trace are ERR_RANGE"
if $cli request 1 --no-wait; then
  echo "a send with no job should have been refused" >&2
  exit 1
fi
if $cli trace 7f3aa1b27f3a --hash-size 3 --no-wait; then
  echo "a trace with three-byte hashes should have been refused" >&2
  exit 1
fi
echo "refused, as they should be"

echo
echo "== reply (nothing held on a fresh boot) and counters"
$cli reply
$cli counters

echo
echo "== observations (7). A ring is per-boot RAM, so --adverts hears that many at startup. The"
echo "   pull prints the key prefix, the hops, the SNR, the RSSI and the age; the name comes only"
echo "   with --names, decoded from app_data on this laptop and never on the node."
python3 tools/linkcli.py --pipe "$out/linkloop --adverts 7" obs
python3 tools/linkcli.py --pipe "$out/linkloop --adverts 7" obs 2 --names

echo
echo "== more adverts than the ring holds: status shows it full with a loss count, counters say"
echo "   how many verified, and a pull reports what was lost since the last one"
python3 tools/linkcli.py --pipe "$out/linkloop --adverts 70" status
python3 tools/linkcli.py --pipe "$out/linkloop --adverts 70" counters
python3 tools/linkcli.py --pipe "$out/linkloop --adverts 70" obs 1

echo
echo "== a pull of nothing takes nothing, and an empty ring says so"
python3 tools/linkcli.py --pipe "$out/linkloop --adverts 3" obs 0
$cli obs

echo
echo "== sensor set walter.wake 1 - what a mesh admin sends - raises attention reason 4"
$cli cli "sensor set walter.wake 1"

echo
echo "== sleep, and then that attention on the wire: 0x00 0x00, 300 ms, and the events a second"
echo "   apart with repeat counting up. Four seconds of listening is four of the ten sends."
$cli sleep 600
python3 tools/linkcli.py --pipe "$out/linkloop --wake" listen 4

echo
echo "== the first-boot profile (phase 3's task 03). A boot that found its settings runs no line"
echo "   and says 'never'; --first-boot is a board with none, and the shipped table then runs"
echo "   through upstream's own command interface - which here answers 'set dutycycle $duty' the"
echo "   way the pinned CommonCLI.cpp does, so the profile's reply rule is what judges it."
$cli cli "sensor get firstboot.state"
python3 tools/linkcli.py --pipe "$out/linkloop --first-boot" cli "sensor get firstboot.state"   > "$out/first-boot.log" 2>&1
cat "$out/first-boot.log"
grep -q "> done" "$out/first-boot.log" || {
  echo "a line of the shipped profile was refused by the miniature of upstream's set" >&2
  exit 1
}
echo "   every line of the table taken, the duty-cycle line included - and the one built from the"
echo "   node's key, which with the fixed key of linkloop.cpp is 'set flood.advert.interval 73'"
echo
echo "== and the Danish standard's own refusals, as the pinned CommonCLI.cpp writes them: the"
echo "   profile's reply rule has to reject each of these, or a line that was refused would be"
echo "   counted as done (p3.5-05)"
for pair in "flood.advert.interval 1|Error: interval range is 3-168 hours"             "loop.detect sideways|Error, must be: off, minimal, moderate, or strict"             "flood.max.unscoped 65|Error, max 64"; do
  line="${pair%%|*}"; want="${pair##*|}"
  python3 tools/linkcli.py --pipe "$out/linkloop" cli "set $line" > "$out/refusal.log" 2>&1
  cat "$out/refusal.log"
  grep -qF "$want" "$out/refusal.log" || {
    echo "set $line was not refused with: $want" >&2
    exit 1
  }
done
echo
echo "== and a duty cycle the band could not have is refused, as upstream refuses it"
python3 tools/linkcli.py --pipe "$out/linkloop" cli "set dutycycle 0" > "$out/dutycycle-bad.log" 2>&1
cat "$out/dutycycle-bad.log"
grep -q "ERROR: dutycycle must be 1-100" "$out/dutycycle-bad.log" || {
  echo "set dutycycle 0 was not refused" >&2
  exit 1
}

echo
echo "== provision: the same lines a person types at the USB port, sent one by one over the link"
echo "   (docs/provisioning.md). The file format is the profile's own: # comments, one command"
echo "   line each, sent verbatim."
cat > "$out/provision-ok.txt" <<'EOF'
# A unit of the Danish mesh - docs/provisioning.md
set name TwinOak-Bench
set lat 55.3250
set lon 10.4900
set owner.info Thomas|twinoak.dk
# the default scope of the Danish mesh. These answer with a sentence and not with OK, which is
# a rule of its own in the tool (upstream's CommonCLI::handleRegionCmd).
region default dk
region save
# the admin password, which is never printed back
password sup3rsecret
EOF
python3 tools/linkcli.py --pipe "$out/linkloop" provision "$out/provision-ok.txt" \
  > "$out/provision-ok.log" 2>&1
cat "$out/provision-ok.log"
echo
echo "   and the password's value is nowhere in that output:"
if grep -q "sup3rsecret" "$out/provision-ok.log"; then
  echo "a password line's value was printed" >&2
  exit 1
fi
grep -q "password \*\*\*\*" "$out/provision-ok.log" || {
  echo "the password line was not shown as stars" >&2
  exit 1
}
echo "   not printed, and shown as stars - as it should be"

echo
echo "== provision stops at the first refusal, and says which line it was"
cat > "$out/provision-bad.txt" <<'EOF'
set name TwinOak-Bench
set nonsense 1
set lat 55.3250
EOF
if python3 tools/linkcli.py --pipe "$out/linkloop" provision /dev/null > /dev/null 2>&1; then
  echo "an empty file should not have been provisioned" >&2
  exit 1
fi
printf 'region load\n' > "$out/provision-load.txt"
if python3 tools/linkcli.py --pipe "$out/linkloop" provision "$out/provision-load.txt" \
     > "$out/provision-load.log" 2>&1; then
  echo 'a region load line should have been refused before anything was sent' >&2
  exit 1
fi
cat "$out/provision-load.log"
# The exit code is the tool's and not a pipeline's, so the log is a file and not a `tee`.
if python3 tools/linkcli.py --pipe "$out/linkloop" provision "$out/provision-bad.txt" \
     > "$out/provision-bad.log" 2>&1; then
  cat "$out/provision-bad.log"
  echo "a refused line should have failed the run" >&2
  exit 1
fi
cat "$out/provision-bad.log"
grep -q "stopped at line" "$out/provision-bad.log" || {
  echo "the refusal was not reported" >&2
  exit 1
}
if grep -q "set lat" "$out/provision-bad.log"; then
  echo "the line after the refusal was still sent" >&2
  exit 1
fi
echo "   stopped where it should, and nothing after it was sent"

echo
echo "== provision --dry-run reads the file and sends nothing"
python3 tools/linkcli.py --pipe "$out/linkloop" provision "$out/provision-ok.txt" --dry-run

echo
echo "== reboot 0"
$cli reboot 0 --delay-ms 10

echo
echo "== one pass of the link, timed (the worst case this task can produce)"
"$out/linkloop" --bench 20000

echo
echo "== all of it passed"
