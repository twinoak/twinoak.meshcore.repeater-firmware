#!/usr/bin/env python3
"""Speak the TwinOak box link to a repeater, from a laptop.

The Walter does not exist yet, so this is what talks to the RAK's `Serial1`: a USB-UART dongle on
the adapter's link pins (**3.3 V**) and one sub-command per request this firmware answers. See
`docs/link.md` for the wiring and a worked session.

Every byte it sends and reads goes through `spec/tools/linkref.py` - the reference implementation
of `spec/LINK-PROTOCOL.md`, the same file the fixtures both firmwares test against - so this tool
cannot drift from the specification without the specification moving. It identifies itself with
`peer_kind` 2 (a bench tool) and retries as section 3 says: 1000 ms, the same frame and the same
`seq`, three attempts in all.

    python3 tools/linkcli.py --port COM7 hello
    python3 tools/linkcli.py --port /dev/ttyUSB0 ping
    python3 tools/linkcli.py --port COM7 status
    python3 tools/linkcli.py --port COM7 time
    python3 tools/linkcli.py --port COM7 time set
    python3 tools/linkcli.py --port COM7 identity
    python3 tools/linkcli.py --port COM7 reboot 0
    python3 tools/linkcli.py --port COM7 cli "get radio"
    python3 tools/linkcli.py --port COM7 power
    python3 tools/linkcli.py --port COM7 env
    python3 tools/linkcli.py --port COM7 lpp 5
    python3 tools/linkcli.py --port COM7 sleep 600
    python3 tools/linkcli.py --port COM7 listen

`--pipe <program>` puts a program's stdin and stdout where the serial port would be, which is how
the tool is exercised with no board: `tools/linkloop/` is the firmware's own session behind a pipe.

    python3 tools/linkcli.py --port COM7 job begin <64-hex-key> --path aabb,ccdd
    python3 tools/linkcli.py --port COM7 path
    python3 tools/linkcli.py --port COM7 job end

The mesh client's job (phase 2's task 01): name a peer and a path, read the path back, end the
job.

    python3 tools/linkcli.py --port COM7 login                 # the ACL login, no password
    python3 tools/linkcli.py --port COM7 login -               # read a password from a prompt
    python3 tools/linkcli.py --port COM7 request 1             # a raw request, a type and hex data
    python3 tools/linkcli.py --port COM7 status-of-peer        # request type 1, reply decoded
    python3 tools/linkcli.py --port COM7 neighbours-of-peer    # request type 6, reply decoded
    python3 tools/linkcli.py --port COM7 send-cli "get name"
    python3 tools/linkcli.py --port COM7 discover
    python3 tools/linkcli.py --port COM7 trace 7f3aa1b27f3a46f6
    python3 tools/linkcli.py --port COM7 reply                 # the reply record as it stands
    python3 tools/linkcli.py --port COM7 counters              # MESH_COUNTERS

The five sends (phase 2's task 02): on the job's behalf the RAK logs in to its peer, asks it
something, sends it a CLI line, floods a discovery or traces a path, and reports the reply as an
`EV_MESH_REPLY` event (which `listen` shows) and on `reply`. Each send is one packet on the air.
A password is read from a prompt, never the command line, and printed as stars.
Requires `pyserial` for `--port` and nothing at all for `--pipe` or `--help`.
"""

import argparse
import getpass
import os
import queue
import shlex
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "spec", "tools"))

import linkref as ref  # noqa: E402  - the path above is what makes this import work

PEER_KIND_BENCH_TOOL = 2
OUR_VERSION = "linkcli 1.0"

# spec/LINK-PROTOCOL.md section 10.
RESPONSE_TIMEOUT_S = 1.0
ATTEMPTS = 3
IDLE_BEFORE_LEADING_ZERO_S = 0.100

STATUS_NAMES = {
    0: "OK", 1: "ERR_UNSUPPORTED", 2: "ERR_MALFORMED", 3: "ERR_BUSY", 4: "ERR_STATE",
    5: "ERR_RANGE", 6: "ERR_FAILED", 7: "ERR_DENIED",
}
ROLE_NAMES = {0: "unknown", 1: "repeater", 2: "bridge", 3: "scanner"}
CAP_NAMES = [
    (ref.CAP_CLI, "CLI"), (ref.CAP_POWER, "POWER"), (ref.CAP_ENV, "ENV"),
    (ref.CAP_MESH_CLIENT, "MESH_CLIENT"), (ref.CAP_OBSERVATIONS, "OBSERVATIONS"),
    (ref.CAP_ATTENTION, "ATTENTION"), (ref.CAP_TIME_PERSIST, "TIME_PERSIST"),
]
RESET_NAMES = {
    0: "unknown", 1: "power-on", 2: "reset pin", 3: "watchdog", 4: "software", 5: "lockup",
    6: "wake from system-off", 7: "left the bootloader",
}
TIME_STATE_NAMES = {0: "unset", 1: "restored", 2: "set", 3: "rtc"}
STATUS_FLAG_NAMES = [
    (0x0001, "job begun"), (0x0002, "reply awaited"), (0x0004, "walter asleep"),
    (0x0008, "adverts held"), (0x0010, "radio up"),
]
TIME_SOURCE_NAMES = {1: "the cellular network", 2: "the server", 3: "a person at a console"}
# A path's path_mode (LINK-PROTOCOL.md 6.1).
PATH_MODE_NAMES = {0: "none (the next send floods)", 1: "direct (zero hops)", 2: "hops"}
# POWER_GET's three channels on the standard B-board, what a positive current means on each, and
# the shunt behind it (LINK-PROTOCOL.md 5.8; the hardware repository's CURRENT-SENSE.md).
POWER_CHANNEL_NAMES = [
    ("panel", "into the charger", 100),
    ("battery", "charging", 20),
    ("load", "drawn", 50),
]
POWER_VALID_NAMES = [(0x01, "channel 1"), (0x02, "channel 2"), (0x04, "channel 3")]
ENV_VALID_NAMES = [(0x01, "temperature"), (0x02, "humidity"), (0x04, "pressure")]
# LPP_GET's perm_mask: MeshCore's telemetry permission bits.
LPP_PERM_NAMES = [(0x01, "base"), (0x02, "location"), (0x04, "environment")]
# The LPP types this firmware writes, and how to read one (src/power/Telemetry.h; MeshCore's
# src/helpers/sensors/LPPDataHelpers.h). Each is two bytes, most significant first.
LPP_TYPES = {
    103: ("temperature", 10.0, True, "C"),
    116: ("voltage", 100.0, False, "V"),
    117: ("current", 1000.0, True, "A"),
}
# The LPP channels of a TwinOak repeater - chosen once and never renumbered (docs/link.md).
LPP_CHANNEL_NAMES = {1: "self", 2: "panel", 3: "battery", 4: "load", 5: "enclosure"}
# EV_ATTENTION's reason, in words, with what its `detail` then means (LINK-PROTOCOL.md 8).
ATTENTION_REASON_NAMES = {
    1: ("the node restarted", "the reset reason"),
    2: ("battery low", "battery millivolts"),
    3: ("battery critical", "battery millivolts"),
    4: ("a person asked for the Walter", "nothing"),
    5: ("the observation ring is nearly full", "records waiting"),
    6: ("a fault the node wants reported", "a code of the firmware's own"),
}
# The console commands that restart the node and never answer (src/helpers/CommonCLI.cpp at the
# pin): silence after one of them is the expected outcome and not a dead link (section 5.7).
CLI_NO_ANSWER = ("reboot", "poweroff", "shutdown", "clkreboot")
TIME_FORCE = 0x01  # TIME_SET's flags, bit 0 (LINK-PROTOCOL.md 5.4)
REBOOT_MODE_NAMES = {
    0: "restart the application", 1: "UF2 bootloader", 2: "serial DFU", 3: "over-the-air DFU",
}
# A send response's `send` (LINK-PROTOCOL.md 6.2) and a reply record's `state` (6.3).
SEND_RESULT_NAMES = {0: "nothing sent", 1: "sent as a flood", 2: "sent direct"}
REPLY_STATE_NAMES = {
    0: "none", 1: "pending", 2: "timeout", 3: "response", 4: "text", 5: "trace",
}
SEND_CLI_SENSITIVE = 0x01  # SEND_CLI's flags, bit 0 (the line holds a password)
# The repeater request types an exit node asks (examples/simple_repeater/MyMesh.cpp at the pin):
# REQ_TYPE_GET_STATUS and REQ_TYPE_GET_NEIGHBOURS, the two `status-of-peer` and `neighbours-of-peer`
# decode for the person at the bench.
REQ_TYPE_GET_STATUS = 0x01
REQ_TYPE_GET_NEIGHBOURS = 0x06


def utc(epoch):
    """An epoch as a date, for a person reading a clock."""
    return time.strftime("%Y-%m-%d %H:%M:%S UTC", time.gmtime(epoch))


def bits(value, names):
    """The names of the bits set in value, and the ones nobody here knows."""
    out = [name for bit, name in names if value & bit]
    known = 0
    for bit, _ in names:
        known |= bit
    rest = value & ~known
    if rest:
        out.append("and 0x%02x nobody here knows" % rest)
    return ", ".join(out) if out else "none"


# ----------------------------------------------------------------------------- the wire


class Wire:
    """A byte pipe to the RAK, and the framing rules of LINK-PROTOCOL.md section 2."""

    def __init__(self, verbose=False):
        self.verbose = verbose
        self._buf = bytearray()
        self._last_tx = 0.0
        self.bad_frames = 0

    # --- what a transport provides ---
    def _write(self, data):
        raise NotImplementedError

    def _read(self, timeout_s):
        """Up to a few bytes, or b'' when the timeout passed."""
        raise NotImplementedError

    def close(self):
        pass

    # --- framing ---
    def send(self, msg_type, seq, payload):
        frame = ref.make_frame(msg_type, seq, payload)
        # "If the line has been idle for 100 ms or more, send one 0x00 before the frame."
        if time.monotonic() - self._last_tx >= IDLE_BEFORE_LEADING_ZERO_S:
            frame = b"\x00" + frame
        if self.verbose:
            print("-> %s" % frame.hex(), file=sys.stderr)
        self._write(frame)
        self._last_tx = time.monotonic()

    def next_frame(self, deadline):
        """The next (type, seq, payload) the RAK sends, or None at the deadline.

        A frame that fails COBS, its length or its CRC is dropped and counted, and nothing is sent
        in reply (section 2).
        """
        while True:
            while True:
                i = self._buf.find(0)
                if i < 0:
                    break
                raw = bytes(self._buf[: i + 1])
                del self._buf[: i + 1]
                if len(raw) == 1:
                    continue  # an empty frame is not an error
                if self.verbose:
                    print("<- %s" % raw.hex(), file=sys.stderr)
                try:
                    return ref.parse_frame(raw)
                except ValueError as exc:
                    self.bad_frames += 1
                    print("dropped a frame (%s)" % exc, file=sys.stderr)
            left = deadline - time.monotonic()
            if left <= 0:
                return None
            chunk = self._read(min(left, 0.05))
            if chunk:
                self._buf += chunk


class SerialWire(Wire):
    def __init__(self, port, baud, verbose=False):
        super().__init__(verbose)
        try:
            import serial  # pyserial, imported here so that --help and --pipe need nothing
        except ImportError:
            raise SystemExit(
                "this needs pyserial for --port: python3 -m pip install pyserial"
            )
        self._port = serial.Serial(port=port, baudrate=baud, bytesize=8,
                                   parity=serial.PARITY_NONE, stopbits=1, timeout=0,
                                   rtscts=False, dsrdtr=False)

    def _write(self, data):
        self._port.write(data)
        self._port.flush()

    def _read(self, timeout_s):
        self._port.timeout = timeout_s
        return self._port.read(256) or b""

    def close(self):
        self._port.close()


class PipeWire(Wire):
    """A program's stdin and stdout where the serial port would be: tools/linkloop/.

    A reader thread puts bytes on a queue, so that a timeout here means the same thing it means
    on a serial port - the other end said nothing - rather than a block for ever.
    """

    def __init__(self, command, verbose=False):
        super().__init__(verbose)
        self._proc = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                      bufsize=0)
        self._queue = queue.Queue()
        self._reader = threading.Thread(target=self._pump, daemon=True)
        self._reader.start()

    def _pump(self):
        while True:
            b = self._proc.stdout.read(1)
            if not b:
                return
            self._queue.put(b)

    def _write(self, data):
        self._proc.stdin.write(data)
        self._proc.stdin.flush()

    def _read(self, timeout_s):
        try:
            return self._queue.get(timeout=max(timeout_s, 0.001))
        except queue.Empty:
            return b""

    def close(self):
        try:
            self._proc.stdin.close()
            self._proc.wait(timeout=2)
        except Exception:
            self._proc.kill()


# ----------------------------------------------------------------------------- the session


class Link:
    """One session with the RAK: the seq, the retries, and events skipped while waiting."""

    def __init__(self, wire):
        self.wire = wire
        self._seq = 0
        self.events = []
        self.last_status = None  # the status byte of the last response, for a caller's messages

    def _next_seq(self):
        # "The Walter picks seq for each new request, any value different from the previous
        # request's" (section 3).
        self._seq = (self._seq + 1) & 0xFF
        return self._seq

    def request(self, msg_type, fields=None, attempts=ATTEMPTS, timeout_s=RESPONSE_TIMEOUT_S):
        """Sends one request and returns its decoded response fields.

        On silence it sends the same frame again, same seq, up to `attempts` times in all; after
        that the link is down as far as this tool is concerned (section 3, section 9).
        """
        payload = ref.encode("request", msg_type, fields or {})
        seq = self._next_seq()
        want = msg_type | ref.RESPONSE_BIT
        for attempt in range(1, attempts + 1):
            if attempt > 1:
                print("no answer; attempt %d of %d" % (attempt, attempts), file=sys.stderr)
            self.wire.send(msg_type, seq, payload)
            deadline = time.monotonic() + timeout_s
            while True:
                got = self.wire.next_frame(deadline)
                if got is None:
                    break
                got_type, got_seq, got_payload = got
                if ref.EV_BOOT <= got_type <= 0x7F:
                    self._note_event(got_type, got_seq, got_payload)
                    continue  # an event is never the answer to a request
                if got_type != want or got_seq != seq:
                    print("ignored type 0x%02x seq %d" % (got_type, got_seq), file=sys.stderr)
                    continue
                fields = ref.decode("response", msg_type, got_payload)
                self.last_status = fields.get("status")
                return fields
        raise SystemExit(
            "the link is down: %d attempts at %s went unanswered"
            % (attempts, ref.NAMES.get(msg_type, "0x%02x" % msg_type))
        )

    def _note_event(self, msg_type, seq, payload):
        fields = ref.decode("event", msg_type, payload)
        self.events.append((msg_type, seq, fields))

    def listen(self, seconds):
        """Prints every event that arrives, until the time is up (or for ever with 0)."""
        deadline = time.monotonic() + seconds if seconds else float("inf")
        while time.monotonic() < deadline:
            got = self.wire.next_frame(min(deadline, time.monotonic() + 0.5))
            if got is None:
                continue
            msg_type, seq, payload = got
            name = ref.NAMES.get(msg_type, "0x%02x" % msg_type)
            if ref.EV_BOOT <= msg_type <= 0x7F:
                fields = ref.decode("event", msg_type, payload)
                print("%s seq %d" % (name, seq))
                print_fields(msg_type, fields, indent="  ")
            else:
                print("not an event: type 0x%02x seq %d" % (msg_type, seq))


# ----------------------------------------------------------------------------- printing


def print_hello_body(f, indent=""):
    print("%sprotocol      %d.%d" % (indent, f["proto_major"], f["proto_minor"]))
    print("%srole          %d (%s)" % (indent, f["role"], ROLE_NAMES.get(f["role"], "?")))
    print("%sslot          %d" % (indent, f["slot"]))
    print("%smax_payload   %d" % (indent, f["max_payload"]))
    print("%scaps          0x%02x (%s)" % (indent, f["caps"], bits(f["caps"], CAP_NAMES)))
    print("%sboot_id       0x%08x" % (indent, f["boot_id"]))
    print("%sfw_version    %s" % (indent, f["fw_version"]))
    print("%sboard         %s" % (indent, f["board"]))
    print("%supstream      %s" % (indent, f["upstream"]))


def print_fields(msg_type, f, indent=""):
    if "status" in f and f["status"] != ref.OK:
        print("%sstatus        %d (%s)"
              % (indent, f["status"], STATUS_NAMES.get(f["status"], "?")))
        return
    if msg_type in (ref.REBOOT, ref.SLEEP_NOTICE, ref.JOB_BEGIN, ref.JOB_END, ref.PATH_SET):
        # "When status is not OK the payload is that one byte and nothing else" (section 3), and
        # these five have no body when it is OK either.
        print('%sstatus        0 (OK)' % indent)
    elif msg_type == ref.PATH_GET:
        print("%spath_mode     %d (%s)"
              % (indent, f["path_mode"], PATH_MODE_NAMES.get(f["path_mode"], "?")))
        if f["path_mode"] == 2:
            print("%shash_size     %d" % (indent, f["hash_size"]))
            print("%shop_count     %d" % (indent, f["hop_count"]))
            print("%shashes        %s" % (indent, f["hashes"]))
    elif msg_type == ref.HELLO:
        print_hello_body(f, indent)
    elif msg_type == ref.PING:
        print("%snonce         0x%08x" % (indent, f["nonce"]))
        print("%suptime_s      %d" % (indent, f["uptime_s"]))
    elif msg_type == ref.STATUS:
        print("%suptime_s      %d" % (indent, f["uptime_s"]))
        print("%sreset_reason  %d (%s)"
              % (indent, f["reset_reason"], RESET_NAMES.get(f["reset_reason"], "?")))
        print("%stime_state    %d (%s)"
              % (indent, f["time_state"], TIME_STATE_NAMES.get(f["time_state"], "?")))
        print("%sepoch         %d" % (indent, f["epoch"]))
        print("%sflags         0x%04x (%s)"
              % (indent, f["flags"], bits(f["flags"], STATUS_FLAG_NAMES)))
        print("%sobs_pending   %d" % (indent, f["obs_pending"]))
        print("%sobs_dropped   %d" % (indent, f["obs_dropped"]))
        print("%slink_errors   %d" % (indent, f["link_errors"]))
        print("%sattention     %d" % (indent, f["attention"]))
    elif msg_type == ref.TIME_GET:
        print("%sepoch         %d (%s)" % (indent, f["epoch"], utc(f["epoch"])))
        print("%stime_state    %d (%s)"
              % (indent, f["time_state"], TIME_STATE_NAMES.get(f["time_state"], "?")))
    elif msg_type == ref.TIME_SET:
        print("%sprev_epoch    %d (%s)" % (indent, f["prev_epoch"], utc(f["prev_epoch"])))
        print("%sepoch         %d (%s)" % (indent, f["epoch"], utc(f["epoch"])))
        print("%stime_state    %d (%s)"
              % (indent, f["time_state"], TIME_STATE_NAMES.get(f["time_state"], "?")))
    elif msg_type == ref.IDENTITY:
        print("%spubkey        %s" % (indent, f["pubkey"]))
        print("%sname          %s" % (indent, f["name"]))
    elif msg_type == ref.EV_BOOT:
        print("%sreset_reason  %d (%s)"
              % (indent, f["reset_reason"], RESET_NAMES.get(f["reset_reason"], "?")))
        print_hello_body(f, indent)
    elif msg_type == ref.CLI:
        # The reply text as the console would have printed it, newlines and all - and nothing
        # added to it, because upstream's own replies already begin with their own "> ".
        text = f["text"]
        if text == "":
            print("%s(no reply)" % indent)
        else:
            for piece in text.split("\n"):
                print("%s%s" % (indent, piece))
    elif msg_type == ref.POWER_GET:
        print("%svalid         0x%02x (%s)"
              % (indent, f["valid"], bits(f["valid"], POWER_VALID_NAMES)))
        for i, ch in enumerate(f["channels"]):
            name, positive, shunt = POWER_CHANNEL_NAMES[i]
            if not f["valid"] & (1 << i):
                print("%s%d %-8s  (not read)" % (indent, i + 1, name))
                continue
            print("%s%d %-8s  %6d mV  %+7d mA   %d mohm, + = %s"
                  % (indent, i + 1, name, ch["bus_mv"], ch["current_ma"], shunt, positive))
    elif msg_type == ref.ENV_GET:
        print("%svalid         0x%02x (%s)"
              % (indent, f["valid"], bits(f["valid"], ENV_VALID_NAMES)))
        print("%stemperature   %.2f C" % (indent, f["temp_centi"] / 100.0))
        print("%shumidity      %.2f %%" % (indent, f["rh_centi"] / 100.0))
        print("%spressure      %d Pa (%.2f hPa)"
              % (indent, f["pressure_pa"], f["pressure_pa"] / 100.0))
    elif msg_type == ref.LPP_GET:
        print("%s%d byte(s): %s" % (indent, len(f["lpp"]) // 2, f["lpp"]))
        print_lpp(f["lpp"], indent)
    elif msg_type in (ref.SEND_LOGIN, ref.SEND_REQUEST, ref.SEND_CLI, ref.SEND_DISCOVERY,
                      ref.SEND_TRACE):
        print("%ssend          %d (%s)"
              % (indent, f["send"], SEND_RESULT_NAMES.get(f["send"], "?")))
        print("%stag           0x%08x%s"
              % (indent, f["tag"], "  (the next reply from the peer)" if f["tag"] == 0 else ""))
        print("%ssender_ts     %d (%s)" % (indent, f["sender_ts"], utc(f["sender_ts"])))
    elif msg_type in (ref.REPLY_GET, ref.EV_MESH_REPLY):
        print_reply(f, indent)
    elif msg_type == ref.MESH_COUNTERS:
        print("%srx            %d" % (indent, f["rx"]))
        print("%stx            %d" % (indent, f["tx"]))
        print("%sclient_tx     %d  (packets the mesh client originated)" % (indent, f["client_tx"]))
        print("%sadverts       %d" % (indent, f["adverts"]))
        print("%sadverts_drop  %d" % (indent, f["adverts_dropped"]))
        print("%sobs_dropped   %d" % (indent, f["obs_dropped"]))
        print("%snoise_floor   %d dBm" % (indent, f["noise_floor"]))
        print("%slast_rssi     %d dBm" % (indent, f["last_rssi"]))
        print("%slast_snr      %.2f dB" % (indent, f["last_snr_q4"] / 4.0))
    elif msg_type == ref.EV_ATTENTION:
        reason, detail = ATTENTION_REASON_NAMES.get(f["reason"], ("?", "?"))
        print("%sreason        %d (%s)" % (indent, f["reason"], reason))
        print("%sdetail        %d (%s)" % (indent, f["detail"], detail))
        print("%srepeat        %d of %d" % (indent, f["repeat"], 10))
    else:
        for key in sorted(f):
            print("%s%-13s %s" % (indent, key, f[key]))


def print_reply(f, indent=""):
    """A reply record (LINK-PROTOCOL.md 6.3): REPLY_GET's body and EV_MESH_REPLY's payload."""
    state = f["state"]
    print("%sstate         %d (%s)" % (indent, state, REPLY_STATE_NAMES.get(state, "?")))
    if state == ref.REPLY_NONE:
        return
    print("%stag           0x%08x" % (indent, f["tag"]))
    if f.get("via_path"):
        print("%svia_path      1  (the reply came back inside a returned path)" % indent)
    rssi = f["rssi"]
    print("%ssnr           %.2f dB" % (indent, f["snr_q4"] / 4.0))
    print("%srssi          %s" % (indent, "unknown" if rssi == 0x7FFF else "%d dBm" % rssi))
    print("%selapsed_ms    %d" % (indent, f["elapsed_ms"]))
    if state == ref.REPLY_RESPONSE:
        print("%sdata          %s" % (indent, f["data"] or "(none)"))
    elif state == ref.REPLY_TEXT:
        for piece in f["text"].split("\n"):
            print("%s%s" % (indent, piece))
    elif state == ref.REPLY_TRACE:
        size = 1 << (f["trace_flags"] & 0x03)
        hashes = f["hashes"]
        snrs = bytes.fromhex(f["snrs"])
        hops = [hashes[i:i + size * 2] for i in range(0, len(hashes), size * 2)]
        print("%strace_flags   0x%02x  (%d-byte hashes)" % (indent, f["trace_flags"], size))
        print("%sfinal_snr     %.2f dB" % (indent, f["final_snr_q4"] / 4.0))
        for i, hop in enumerate(hops):
            snr = "" if i >= len(snrs) else "  snr %.2f dB" % (
                (snrs[i] - 256 if snrs[i] > 127 else snrs[i]) / 4.0)
            print("%s  hop %-2d %s%s" % (indent, i, hop, snr))


def print_lpp(data, indent=""):
    """The Cayenne-LPP bytes, read back field by field - which is what a phone's telemetry view of
    this repeater shows (docs/link.md has the channel table)."""
    raw = bytes.fromhex(data) if isinstance(data, str) else bytes(data)
    if not raw:
        print("%s(no fields)" % indent)
        return
    i = 0
    while i + 2 <= len(raw):
        channel, kind = raw[i], raw[i + 1]
        i += 2
        name, mult, signed, unit = LPP_TYPES.get(kind, (None, None, None, None))
        if name is None or i + 2 > len(raw):
            print("%schannel %d: type %d, which this tool cannot read" % (indent, channel, kind))
            return
        value = int.from_bytes(raw[i:i + 2], "big", signed=signed)
        i += 2
        print("%schannel %-2d %-10s %-11s %8.3f %s"
              % (indent, channel, LPP_CHANNEL_NAMES.get(channel, "?"), name, value / mult, unit))
    if i != len(raw):
        print("%s%d byte(s) left over" % (indent, len(raw) - i))


def report_events(link):
    for msg_type, seq, fields in link.events:
        print("(also heard %s seq %d)" % (ref.NAMES.get(msg_type, "?"), seq))
        print_fields(msg_type, fields, indent="  ")
    link.events = []


# ----------------------------------------------------------------------------- the commands


def cmd_hello(link, args):
    f = link.request(ref.HELLO, {
        "proto_major": ref.PROTO_MAJOR,
        "proto_minor": ref.PROTO_MINOR,
        "peer_kind": PEER_KIND_BENCH_TOOL,
        "max_payload": ref.MAX_PAYLOAD,
        "fw_version": OUR_VERSION,
    })
    print("HELLO")
    print_fields(ref.HELLO, f, indent="  ")
    return 0 if f.get("status") == ref.OK else 1


def cmd_ping(link, args):
    nonce = args.nonce if args.nonce is not None else int(time.time()) & 0xFFFFFFFF
    started = time.monotonic()
    f = link.request(ref.PING, {"nonce": nonce})
    elapsed_ms = (time.monotonic() - started) * 1000.0
    print("PING in %.1f ms" % elapsed_ms)
    print_fields(ref.PING, f, indent="  ")
    if f.get("status") == ref.OK and f.get("nonce") != nonce:
        print("the nonce came back wrong: sent 0x%08x" % nonce, file=sys.stderr)
        return 1
    return 0 if f.get("status") == ref.OK else 1


def cmd_status(link, args):
    f = link.request(ref.STATUS, {})
    print("STATUS")
    print_fields(ref.STATUS, f, indent="  ")
    return 0 if f.get("status") == ref.OK else 1


TIME_USAGE = "usage: time [set [epoch]] [--force] [--source 1|2|3]"


def cmd_time(link, args):
    # The flags, from wherever they are in the list (see build_parser).
    words = []
    force = args.force
    source = args.source
    rest = list(args.words)
    while rest:
        word = rest.pop(0)
        if word == "--force":
            force = True
        elif word == "--source" and rest:
            source = int(rest.pop(0), 0)
        elif word in ("-h", "--help"):
            print(TIME_USAGE)
            return 0
        else:
            words.append(word)

    if len(words) > 2 or (words and words[0] != "set"):
        print(TIME_USAGE, file=sys.stderr)
        return 2
    if source not in (1, 2, 3):
        print("--source is 1 (the cellular network), 2 (the server) or 3 (a person at a console)",
              file=sys.stderr)
        return 2

    if not words:
        if force:
            print("--force is for `time set`, which this is not", file=sys.stderr)
            return 2
        f = link.request(ref.TIME_GET, {})
        print("TIME_GET")
        print_fields(ref.TIME_GET, f, indent="  ")
        return 0 if f.get("status") == ref.OK else 1

    epoch = int(words[1], 0) if len(words) == 2 else int(time.time())
    flags = TIME_FORCE if force else 0
    f = link.request(ref.TIME_SET, {"epoch": epoch, "source": source, "flags": flags})
    print("TIME_SET %d (%s), source %d (%s)%s"
          % (epoch, utc(epoch), source, TIME_SOURCE_NAMES.get(source, "?"),
             ", forced" if force else ""))
    print_fields(ref.TIME_SET, f, indent="  ")
    if f.get("status") == ref.ERR_RANGE:
        print("the node refuses a clock that would go backwards unless the state is unset or "
              "restored and nothing with a timestamp has left it since it booted; `--force` is "
              "the override, and it is for a person's instruction (LINK-PROTOCOL.md 5.4)",
              file=sys.stderr)
    return 0 if f.get("status") == ref.OK else 1


def cmd_identity(link, args):
    f = link.request(ref.IDENTITY, {})
    print("IDENTITY")
    print_fields(ref.IDENTITY, f, indent="  ")
    return 0 if f.get("status") == ref.OK else 1


def cmd_reboot(link, args):
    f = link.request(ref.REBOOT, {"mode": args.mode, "delay_ms": args.delay_ms})
    print("REBOOT mode %d (%s), delay %d ms"
          % (args.mode, REBOOT_MODE_NAMES.get(args.mode, "?"), args.delay_ms))
    print_fields(ref.REBOOT, f, indent="  ")
    if f.get("status") != ref.OK:
        return 1
    print("the node answered first and restarts after the delay; modes 1 to 3 hand the radio to a")
    print("bootloader this link cannot talk to (LINK-PROTOCOL.md 5.5)")
    return 0


def cmd_cli(link, args):
    """One console command line, run on the node with console privileges (5.7)."""
    line = " ".join(args.words).strip()
    if not line:
        print('usage: cli "<command line>"   (1 to 160 bytes of UTF-8)', file=sys.stderr)
        return 2
    encoded = line.encode("utf-8")
    if len(encoded) > 160:
        print("that line is %d bytes; the protocol takes 160" % len(encoded), file=sys.stderr)
        return 2

    # Upstream tests those four words with memcmp against the start of the line, so anything
    # beginning with one of them restarts the node (`reboot now` does).
    restarts = any(line.startswith(word) for word in CLI_NO_ANSWER)
    print("CLI %s" % line)
    try:
        f = link.request(ref.CLI, {"text": line})
    except (SystemExit, OSError, ValueError):
        # "Some commands restart the node and never answer; the Walter treats silence after such
        # a line as the expected outcome, not as a dead link" (5.7). On a real link that is
        # silence and three attempts (SystemExit); through a pipe the program is gone and the
        # pipe breaks under the retry, which means the same thing.
        if restarts:
            print("  (no answer, which is what `%s` means: the node restarted)" % line)
            return 0
        raise
    print_fields(ref.CLI, f, indent="  ")
    if f.get("status") == ref.ERR_MALFORMED:
        print("a line is 1 to 160 bytes of UTF-8 text with no control characters in it",
              file=sys.stderr)
    return 0 if f.get("status") == ref.OK else 1


def cmd_power(link, args):
    """The three INA3221 channels, each with the board's own shunt already applied (5.8)."""
    f = link.request(ref.POWER_GET, {})
    print("POWER_GET")
    print_fields(ref.POWER_GET, f, indent="  ")
    if f.get("status") != ref.OK:
        return 1
    if f["valid"] == 0:
        print("nothing was read: the INA3221 at 0x42 is not answering (check the ribbon to the")
        print("B-board and its always-on 3.3 V) - and the repeater keeps repeating regardless")
    elif f["channels"][1]["current_ma"] < 0:
        print("channel 2 is negative, so the pack is **discharging** (5.8)")
    return 0


def cmd_env(link, args):
    """The air inside the enclosure (5.8)."""
    f = link.request(ref.ENV_GET, {})
    print("ENV_GET")
    print_fields(ref.ENV_GET, f, indent="  ")
    if f.get("status") != ref.OK:
        return 1
    if f["valid"] == 0:
        print("nothing was read: no BME280 answered at 0x76 on this unit")
    return 0


def cmd_lpp(link, args):
    """Exactly what a mesh telemetry request at those permissions would be answered with."""
    f = link.request(ref.LPP_GET, {"perm_mask": args.mask})
    print("LPP_GET perm_mask 0x%02x (%s)" % (args.mask, bits(args.mask, LPP_PERM_NAMES)))
    print_fields(ref.LPP_GET, f, indent="  ")
    return 0 if f.get("status") == ref.OK else 1


def cmd_sleep(link, args):
    """Tell the node this end is going to sleep (8): it then sends no events but EV_BOOT and
    EV_ATTENTION, and says so in STATUS's flag bit 2, until the next request."""
    f = link.request(ref.SLEEP_NOTICE, {"expect_back_s": args.seconds})
    print("SLEEP_NOTICE, back in %s"
          % ("%d s" % args.seconds if args.seconds else "an unknown time (0)"))
    print_fields(ref.SLEEP_NOTICE, f, indent="  ")
    if f.get("status") != ref.OK:
        return 1
    print("the node believes this end is asleep until the next request; `listen` now shows")
    print("only EV_BOOT and EV_ATTENTION, and any request at all wakes it again")
    return 0


def _parse_path(args):
    """The path fields a JOB_BEGIN or PATH_SET carries, from --direct / --path (6.1)."""
    if args.direct and args.path:
        raise SystemExit("give --direct or --path, not both")
    if args.direct:
        return {"path_mode": ref.PATH_DIRECT}
    if not args.path:
        return {"path_mode": ref.PATH_NONE}  # flood
    hops = [h.strip() for h in args.path.split(",") if h.strip()]
    if not 1 <= len(hops) <= 63:
        raise SystemExit("a path is 1 to 63 hops")
    sizes = {len(h) // 2 for h in hops}
    if len(sizes) != 1 or any(len(h) % 2 for h in hops):
        raise SystemExit("every hop is the same number of whole bytes of hex")
    size = sizes.pop()
    if not 1 <= size <= 3:
        raise SystemExit("a hop hash is 1 to 3 bytes")
    return {
        "path_mode": ref.PATH_HOPS,
        "hash_size": size,
        "hop_count": len(hops),
        "hashes": "".join(hops),
    }


def cmd_job(link, args):
    """Begin or end the mesh client's job (6.1): one peer, one path, nothing on the air yet."""
    if args.what == "end":
        f = link.request(ref.JOB_END, {})
        print("JOB_END")
        print_fields(ref.JOB_END, f, indent="  ")
        return 0 if f.get("status") == ref.OK else 1

    key = (args.key or "").lower()
    if len(key) != 64 or any(c not in "0123456789abcdef" for c in key):
        print("job begin needs the peer's 64-hex-character public key", file=sys.stderr)
        return 2
    fields = {"pubkey": key}
    fields.update(_parse_path(args))
    f = link.request(ref.JOB_BEGIN, fields)
    print("JOB_BEGIN %s, path %s"
          % (key[:8] + "...", PATH_MODE_NAMES.get(fields["path_mode"], "?")))
    print_fields(ref.JOB_BEGIN, f, indent="  ")
    if f.get("status") == ref.OK:
        print("the peer is set; `path` reads the route, and task 02's sends put one packet on the")
        print("air each (nothing is transmitted yet)")
    return 0 if f.get("status") == ref.OK else 1


def cmd_path(link, args):
    """The path the job holds now (6.1). A reply that returned a path changes it (task 02)."""
    f = link.request(ref.PATH_GET, {})
    print("PATH_GET")
    print_fields(ref.PATH_GET, f, indent="  ")
    if f.get("status") == ref.ERR_STATE:
        print("no job: `job begin <key>` first", file=sys.stderr)
    return 0 if f.get("status") == ref.OK else 1


def _send_result(link, msg_type, fields, name):
    """One of the five sends: print the answer, and - unless --no-wait - the reply when it comes."""
    f = link.request(msg_type, fields)
    print(name)
    print_fields(msg_type, f, indent="  ")
    if f.get("status") != ref.OK:
        if f.get("status") == ref.ERR_STATE:
            print("no job: `job begin <key>` first (6.2)", file=sys.stderr)
        elif f.get("status") == ref.ERR_BUSY:
            print("a reply is still awaited; `reply` reads it, or wait for it (6.2)",
                  file=sys.stderr)
        return 1
    return 0


def _await_reply(link, args):
    """Poll REPLY_GET until the record leaves `pending`, or the timeout plus a margin passes
    (6.3: the Walter runs its own timer and asks with REPLY_GET if the event did not come)."""
    if args.timeout_ms == 0 or args.no_wait:
        return 0
    deadline = time.monotonic() + args.timeout_ms / 1000.0 + 1.0
    while time.monotonic() < deadline:
        f = link.request(ref.REPLY_GET, {})
        if f.get("status") == ref.OK and f.get("state") != ref.REPLY_PENDING:
            print("REPLY")
            print_fields(ref.REPLY_GET, f, indent="  ")
            return 0 if f.get("state") not in (ref.REPLY_NONE, ref.REPLY_TIMEOUT) else 1
        time.sleep(0.2)
    print("no reply within the timeout; `reply` reads the record", file=sys.stderr)
    return 1


def cmd_login(link, args):
    """A login as ourselves (6.2): the ACL login with no password, or a guest/admin login with
    one read from a prompt when the argument is `-`."""
    password = ""
    if args.password == "-":
        password = getpass.getpass("peer password (not echoed): ")
    elif args.password is not None:
        password = args.password
    data = password.encode("utf-8")
    if len(data) > 15:
        print("a password is at most 15 bytes (6.2)", file=sys.stderr)
        return 2
    shown = "(the ACL login, no password)" if not data else "*" * len(data)
    print("SEND_LOGIN %s" % shown)
    rc = _send_result(link, ref.SEND_LOGIN,
                      {"timeout_ms": args.timeout_ms, "password": data.hex()}, "")
    return rc or _await_reply(link, args)


def cmd_request(link, args):
    """A raw request to the peer (6.2): a type and optional hex data, the reply matched by the
    timestamp."""
    data = args.data or ""
    try:
        bytes.fromhex(data)
    except ValueError:
        print("data is hex bytes, e.g. 00090000080011223344", file=sys.stderr)
        return 2
    print("SEND_REQUEST type %d" % args.type)
    rc = _send_result(link, ref.SEND_REQUEST,
                      {"timeout_ms": args.timeout_ms, "req_type": args.type, "data": data}, "")
    return rc or _await_reply(link, args)


def cmd_status_of_peer(link, args):
    """A convenience: request type 1 (REQ_TYPE_GET_STATUS) and the reply read as a repeater's
    stats block (6.2)."""
    print("SEND_REQUEST status-of-peer (type 1)")
    rc = _send_result(link, ref.SEND_REQUEST,
                      {"timeout_ms": args.timeout_ms, "req_type": REQ_TYPE_GET_STATUS, "data": ""},
                      "")
    if rc:
        return rc
    return _await_reply(link, args)


def cmd_neighbours_of_peer(link, args):
    """A convenience: request type 6 (REQ_TYPE_GET_NEIGHBOURS) with the page params the companion
    sends, and the reply (6.2)."""
    # count 9, offset 0, order newest-first, an eight-byte prefix, and a four-byte random blob.
    data = "09" + "0000" + "00" + "08" + "11223344"
    print("SEND_REQUEST neighbours-of-peer (type 6)")
    rc = _send_result(link, ref.SEND_REQUEST,
                      {"timeout_ms": args.timeout_ms, "req_type": REQ_TYPE_GET_NEIGHBOURS,
                       "data": data}, "")
    if rc:
        return rc
    return _await_reply(link, args)


def cmd_send_cli(link, args):
    """One CLI line to the peer (6.2), run on *it* with the session's privileges. --sensitive
    flags a line that holds a password so the RAK wipes it; `reply` carries the peer's answer."""
    line = " ".join(args.words).strip()
    if not line:
        print('usage: send-cli "<command line>"', file=sys.stderr)
        return 2
    encoded = line.encode("utf-8")
    if len(encoded) > 148:
        print("that line is %d bytes; a mesh CLI line takes 148 (6.2)" % len(encoded),
              file=sys.stderr)
        return 2
    flags = SEND_CLI_SENSITIVE if args.sensitive else 0
    print("SEND_CLI %s%s" % ("(sensitive) " if args.sensitive else "", line))
    rc = _send_result(link, ref.SEND_CLI,
                      {"timeout_ms": args.timeout_ms, "flags": flags, "text": line}, "")
    return rc or _await_reply(link, args)


def cmd_discover(link, args):
    """A path discovery (6.2): a base-telemetry request, always flooded, whose reply comes back as
    a returned path and teaches the route. `path` reads it afterwards."""
    print("SEND_DISCOVERY")
    rc = _send_result(link, ref.SEND_DISCOVERY, {"timeout_ms": args.timeout_ms}, "")
    return rc or _await_reply(link, args)


def cmd_trace(link, args):
    """A trace along a round-trip path (6.2), always direct, that comes back with an SNR per hop.
    The hashes are hex, each hop 1, 2, 4 or 8 bytes - never 3, which a trace cannot encode."""
    hashes = (args.hashes or "").strip()
    try:
        raw = bytes.fromhex(hashes)
    except ValueError:
        print("the hashes are hex bytes of the round-trip path", file=sys.stderr)
        return 2
    if not raw or len(raw) % args.hash_size != 0:
        print("the hashes must be whole %d-byte hops" % args.hash_size, file=sys.stderr)
        return 2
    print("SEND_TRACE %d-byte hops: %s" % (args.hash_size, hashes))
    rc = _send_result(link, ref.SEND_TRACE,
                      {"timeout_ms": args.timeout_ms, "hash_size": args.hash_size,
                       "hashes": hashes}, "")
    if rc and link.last_status == ref.ERR_RANGE and args.hash_size == 3:
        print("a trace cannot carry three-byte hashes (6.2)", file=sys.stderr)
    return rc or _await_reply(link, args)


def cmd_reply(link, args):
    """The reply record as it stands (6.3): pending, an outcome, or nothing held."""
    f = link.request(ref.REPLY_GET, {})
    print("REPLY_GET")
    print_fields(ref.REPLY_GET, f, indent="  ")
    return 0 if f.get("status") == ref.OK else 1


def cmd_counters(link, args):
    """MESH_COUNTERS (6.4): what the radio sent and received, what the client originated, and the
    noise floor and last RSSI/SNR."""
    f = link.request(ref.MESH_COUNTERS, {})
    print("MESH_COUNTERS")
    print_fields(ref.MESH_COUNTERS, f, indent="  ")
    return 0 if f.get("status") == ref.OK else 1


def cmd_listen(link, args):
    if args.seconds:
        print("listening for %d s" % args.seconds)
    else:
        print("listening (control-C to stop)")
    link.listen(args.seconds)
    return 0


def build_parser():
    p = argparse.ArgumentParser(
        prog="linkcli.py",
        description="Speak the TwinOak box link (spec/LINK-PROTOCOL.md 1.0) to a repeater.",
        epilog="docs/link.md has the wiring and a worked session.",
    )
    where = p.add_mutually_exclusive_group(required=True)
    where.add_argument("--port", help="the serial port of the USB-UART dongle (3.3 V!)")
    where.add_argument("--pipe", metavar="PROGRAM",
                       help="run PROGRAM (a command line, split as a shell would) and speak to "
                            "its stdin/stdout instead of a port: tools/linkloop/, which is how "
                            "this tool is tested with no board")
    p.add_argument("--baud", type=int, default=115200, help="default 115200 (the link is fixed)")
    p.add_argument("--attempts", type=int, default=ATTEMPTS,
                   help="attempts at one request; the protocol says 3")
    p.add_argument("--timeout", type=float, default=RESPONSE_TIMEOUT_S,
                   help="seconds to wait for a response; the protocol says 1.0")
    p.add_argument("-v", "--verbose", action="store_true", help="print every frame as hex")

    subs = p.add_subparsers(dest="command", metavar="<command>")

    subs.add_parser("hello", help="what this firmware is (5.1)").set_defaults(run=cmd_hello)
    ping = subs.add_parser("ping", help="prove the main loop is turning (5.2)")
    ping.add_argument("--nonce", type=lambda s: int(s, 0), default=None)
    ping.set_defaults(run=cmd_ping)
    subs.add_parser("status", help="uptime, reset reason, counters (5.3)").set_defaults(
        run=cmd_status)
    t = subs.add_parser("time", help="read the clock, or set it (5.4)")
    # Everything after `time` is taken as one list and read by cmd_time, because argparse fills a
    # second positional only from the arguments *before* an option: with `epoch` declared as a
    # positional, `time set --force <epoch>` - the form docs/link.md documents - would lose the
    # epoch. The two options below are declared all the same, so that `--help` lists them, and
    # cmd_time accepts them wherever they appear.
    t.add_argument("words", nargs=argparse.REMAINDER, metavar="[set [epoch]]",
                   help="nothing reads the clock; `set` sets it, to the epoch given or to this "
                        "laptop's clock")
    t.add_argument("--force", action="store_true",
                   help="let the clock go backwards - a person's instruction, not a Walter's "
                        "(5.4)")
    t.add_argument("--source", type=int, choices=[1, 2, 3], default=3,
                   help="where the time came from: 1 the cellular network, 2 the server, "
                        "3 a person at a console (the default from here)")
    t.set_defaults(run=cmd_time)
    subs.add_parser("identity", help="the node's public key and advert name (5.6)").set_defaults(
        run=cmd_identity)
    reboot = subs.add_parser("reboot", help="restart the node (5.5)")
    reboot.add_argument("mode", type=int, choices=[0, 1, 2, 3],
                        help="0 application, 1 UF2 bootloader, 2 serial DFU, 3 over-the-air DFU")
    reboot.add_argument("--delay-ms", type=int, default=500, dest="delay_ms")
    reboot.set_defaults(run=cmd_reboot)
    c = subs.add_parser("cli", help="run one console command line on the node (5.7)")
    # REMAINDER, for the same reason `time` uses it and so that a line with its own options -
    # `cli "set repeat off"` - needs no quoting games on every shell.
    c.add_argument("words", nargs=argparse.REMAINDER, metavar='"<command line>"',
                   help="the line, exactly as it would be typed on the USB console")
    c.set_defaults(run=cmd_cli)
    subs.add_parser("power", help="the three INA3221 channels, shunts applied (5.8)").set_defaults(
        run=cmd_power)
    subs.add_parser("env", help="temperature, humidity and pressure in the box (5.8)").set_defaults(
        run=cmd_env)
    lpp = subs.add_parser("lpp", help="the Cayenne-LPP a mesh telemetry request would get (5.8)")
    lpp.add_argument("mask", type=lambda v: int(v, 0), nargs="?", default=0x05,
                     help="MeshCore's telemetry permissions: 0x01 base, 0x02 location, "
                          "0x04 environment; the default 0x05 is what an admin asks with")
    lpp.set_defaults(run=cmd_lpp)
    sleep = subs.add_parser("sleep", help="say this end is going to sleep (8)")
    sleep.add_argument("seconds", type=int, nargs="?", default=0,
                       help="how long, for the node's information; 0 (the default) is unknown")
    sleep.set_defaults(run=cmd_sleep)
    job = subs.add_parser("job", help="begin or end the mesh client's job (6.1)")
    job.add_argument("what", choices=["begin", "end"])
    job.add_argument("key", nargs="?", help="the peer's 64-hex-character public key (begin)")
    job.add_argument("--direct", action="store_true",
                     help="the peer is a direct neighbour: zero hops, sent direct")
    job.add_argument("--path", metavar="aabb,ccdd",
                     help="the hops to the peer, comma-separated hex, each 1 to 3 bytes; "
                          "the default with neither flag is a flood")
    job.set_defaults(run=cmd_job)
    subs.add_parser("path", help="the path the job holds now (6.1)").set_defaults(run=cmd_path)

    # The five sends (6.2), the reply (6.3) and the counters (6.4). Each send shares a
    # --timeout-ms and a --no-wait: the RAK awaits for timeout_ms and reports the reply, and the
    # tool polls REPLY_GET until it comes (or --no-wait leaves after the send's own answer).
    def add_send_args(sp):
        sp.add_argument("--timeout-ms", type=int, default=8000, dest="timeout_ms",
                        help="how long the RAK awaits the reply; 0 awaits nothing")
        sp.add_argument("--no-wait", action="store_true", dest="no_wait",
                        help="print the send's own answer and stop, not polling for the reply")

    login = subs.add_parser("login", help="log in to the job's peer as ourselves (6.2)")
    login.add_argument("password", nargs="?", default=None,
                       help="the peer's password; `-` reads it from a prompt; omitted is the ACL "
                            "login with no password")
    add_send_args(login)
    login.set_defaults(run=cmd_login)

    request = subs.add_parser("request", help="a raw request to the peer (6.2)")
    request.add_argument("type", type=lambda v: int(v, 0), help="the request type (e.g. 1)")
    request.add_argument("data", nargs="?", default="", help="request data as hex bytes")
    add_send_args(request)
    request.set_defaults(run=cmd_request)

    sop = subs.add_parser("status-of-peer", help="request type 1 and decode the reply (6.2)")
    add_send_args(sop)
    sop.set_defaults(run=cmd_status_of_peer)

    nop = subs.add_parser("neighbours-of-peer", help="request type 6 and decode the reply (6.2)")
    add_send_args(nop)
    nop.set_defaults(run=cmd_neighbours_of_peer)

    scli = subs.add_parser("send-cli", help="run one CLI line on the peer (6.2)")
    scli.add_argument("words", nargs=argparse.REMAINDER, metavar='"<command line>"',
                      help="the line, run on the peer with the session's privileges")
    scli.add_argument("--sensitive", action="store_true",
                      help="the line holds a password: the RAK wipes it after the send")
    add_send_args(scli)
    scli.set_defaults(run=cmd_send_cli)

    discover = subs.add_parser("discover", help="flood a path discovery to the peer (6.2)")
    add_send_args(discover)
    discover.set_defaults(run=cmd_discover)

    trace = subs.add_parser("trace", help="trace a round-trip path (6.2)")
    trace.add_argument("hashes", help="the round-trip hops as hex, each hop --hash-size bytes")
    trace.add_argument("--hash-size", type=int, choices=[1, 2, 3, 4, 8], default=2,
                       dest="hash_size",
                       help="bytes per hop; 3 is refused (ERR_RANGE), to show a trace cannot "
                            "carry it")
    add_send_args(trace)
    trace.set_defaults(run=cmd_trace)

    subs.add_parser("reply", help="the reply record as it stands (6.3)").set_defaults(
        run=cmd_reply)
    subs.add_parser("counters", help="the mesh counters (6.4)").set_defaults(run=cmd_counters)

    listen = subs.add_parser("listen", help="print the events the node sends")
    listen.add_argument("seconds", type=int, nargs="?", default=0,
                        help="how long; 0 (the default) is for ever")
    listen.set_defaults(run=cmd_listen)
    return p


def main(argv):
    parser = build_parser()
    args = parser.parse_args(argv[1:])
    if not getattr(args, "run", None):
        parser.print_help()
        return 2

    if args.pipe:
        wire = PipeWire(shlex.split(args.pipe, posix=os.name != "nt"), verbose=args.verbose)
    else:
        wire = SerialWire(args.port, args.baud, verbose=args.verbose)
    link = Link(wire)
    try:
        code = args.run(link, args)
        report_events(link)
    except KeyboardInterrupt:
        print("")
        code = 0
    finally:
        wire.close()
    if wire.bad_frames:
        print("%d frame(s) dropped on the way here" % wire.bad_frames, file=sys.stderr)
    return code


if __name__ == "__main__":
    sys.exit(main(sys.argv))
