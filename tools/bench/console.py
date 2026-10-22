"""A console a script can drive - with the rails in the script.

Two consoles live on the board's USB port at 115200, one per role:

  * the repeater's is upstream's own (examples/simple_repeater/main.cpp at the pin, which
    src/roles/repeater/main.cpp copies): a line ends in CR, every character is echoed, and the
    reply follows as `  -> <text>` and a CRLF - several lines for `sensor list`, nothing at all
    for a line that restarts the node or has nothing to say;
  * the scanner's is NDJSON (docs/noisescope.md section 5): a command is a line ending in LF, every
    answer is a JSON object on a line of its own, one of them an `ack` for the command, and a
    `stat` heartbeat arrives every ten seconds whether asked or not.

What this file refuses to send, whatever a caller asks (console_guard):

  * `password ...` and `set guest.password ...` - a line that sets a password - unless the run
    is in the disposable-identity mode docs/bench.md section 10 uses, where the unit is a bench
    unit and the password it gets is one nobody keeps;
  * `erase` under the same rule: it takes the node's identity with it;
  * `set prv.key ...` and `get prv.key`: the node's private key is never set from a laptop and
    never printed (spec/SYSTEM.md S-1, S-12);
  * `reboot 1`, `reboot 2`, `reboot 3` and the scanner's `dfu`: a bootloader this tool does not
    use (no UF2, no over-the-air DFU; its own flash does its own 1200-baud touch);
  * `region load`: upstream's interpreter then waits for a blank line this console does not
    send, and the way out is a reboot (docs/bench.md section 4).

What it does **not** refuse, and why (docs/bench.md section 3, walked by p3.5-04): upstream's own
`reboot` and `clkreboot`. Neither hands the board to a bootloader - `clkreboot` sets upstream's
clock back to its 2024 default and then calls the same `_board->reboot()` as `reboot`
(src/helpers/CommonCLI.cpp:187-190 at the pin d9296435) - neither touches the node's identity,
and the clock keeper restores the saved time on the boot that follows either, so a `clkreboot` of
ours cannot put the node behind its own last advert (src/roles/repeater/clock/ClockKeeper.h).
That is what makes the line checkable by a session rather than a person's: it is the way to see
the keeper work, and it is the whole of docs/bench.md section 3's `clkreboot` line. `reboot 1`,
`2` and `3` stay refused, because those three *are* the bootloader.

And what it never prints: a reply line that begins `password now:` - upstream's `password`
command echoes the new password back (CommonCLI.cpp:260 at the pin) - is replaced before it
reaches the log or the caller, so nothing downstream of this file can see a password.
"""

import json
import re

from config import Refused

REPEATER_PROMPT = b"  -> "
PASSWORD_ECHO = "password now:"
PASSWORD_ECHO_SHOWN = "password now: (not shown by tools/bench)"

DEFAULT_TIMEOUT_S = 2.5
DEFAULT_QUIET_S = 0.15

_HEX64 = re.compile(r"\b([0-9A-Fa-f]{64})\b")


# ----------------------------------------------------------------------------- the guard

def console_guard(line, disposable=False):
    """Raises Refused for a line this tool does not send. Returns the line stripped."""
    if line is None:
        raise Refused("an empty console line is not sent")
    text = line.strip()
    if not text:
        raise Refused("an empty console line is not sent")
    if any(ord(ch) < 0x20 for ch in text):
        raise Refused("a console line with a control character in it is not sent")
    words = text.split()
    head = words[0].lower()
    second = words[1].lower() if len(words) > 1 else ""

    if head == "password":
        if not disposable:
            raise Refused("`password ...` sets the admin password: not sent unless the bench's "
                          "identity is \"disposable\" (bench.local.json), and this one is not")
    if head == "set" and second in ("guest.password", "guest.password:"):
        if not disposable:
            raise Refused("`set guest.password ...` sets a password: not sent unless the bench's "
                          "identity is \"disposable\"")
    if head == "set" and second == "prv.key":
        raise Refused("`set prv.key` would replace the node's private key: never sent")
    if head == "get" and second == "prv.key":
        raise Refused("`get prv.key` prints the node's private key: never sent")
    if head == "erase":
        if not disposable:
            raise Refused("`erase` takes the node's identity with it: not sent unless the bench's "
                          "identity is \"disposable\"")
    if head == "reboot" and second in ("1", "2", "3"):
        raise Refused("`reboot %s` hands the board to a bootloader this tool does not use (UF2, "
                      "serial DFU, OTA): never sent - `bench flash` does its own 1200-baud touch"
                      % second)
    if head == "dfu":
        raise Refused("`dfu` puts the scanner into a bootloader this tool does not use: never sent")
    if head == "region" and second == "load":
        raise Refused("`region load` leaves upstream's interpreter waiting for a blank line this "
                      "console cannot send (docs/bench.md section 4): not sent")
    return text


_REDACTED_STARTS = (PASSWORD_ECHO, "password ", "set guest.password ")


def redact(text):
    """Every line that begins `password now:` replaced - and so is the console's echo of a
    `password ...` or `set guest.password ...` line, which is the same secret typed rather than
    answered. A line whose value is already four stars is left as it is, so that a transcript can
    say which line was sent (walk.py writes `password ****`, as tools/linkcli.py does). The rest
    is untouched."""
    if text is None:
        return None
    out = []
    for line in text.splitlines():
        body = line.strip()
        if body.startswith("->"):
            body = body[2:].strip()
        lowered = body.lower()
        starts = [start for start in _REDACTED_STARTS if lowered.startswith(start)]
        if starts and body[len(starts[0]):].strip() != "****":
            out.append(PASSWORD_ECHO_SHOWN)
        else:
            out.append(line)
    return "\n".join(out)


# ----------------------------------------------------------------------------- the repeater

class Reply(object):
    def __init__(self, line, text, timed_out, raw):
        self.line = line
        self.text = text          # the reply after `  -> `, redacted; "" when there was none
        self.timed_out = timed_out
        self.raw = raw            # everything read, redacted

    @property
    def ok(self):
        return not self.timed_out

    def __repr__(self):
        return "Reply(%r -> %r%s)" % (self.line, self.text, ", timed out" if self.timed_out else "")


def _decode(data):
    return data.decode("utf-8", errors="replace")


class RepeaterConsole(object):
    """One open port to a repeater's USB console."""

    def __init__(self, port, log=None, disposable=False, now=None, sleep=None):
        self.port = port
        self.log = log or (lambda line: None)
        self.disposable = disposable
        import time as _time
        self.now = now or _time.monotonic
        self.sleep = sleep or _time.sleep

    def drain(self, seconds=0.2):
        """Read and discard what is already there (a banner, an echo of an earlier line)."""
        out = b""
        deadline = self.now() + seconds
        while self.now() < deadline:
            chunk = self.port.read(4096, 0.05)
            if chunk:
                out += chunk
        return redact(_decode(out))

    def read_until(self, needle, timeout_s):
        """Bytes until `needle` has been seen (inclusive) or the timeout; (found, data)."""
        data = b""
        deadline = self.now() + timeout_s
        while True:
            left = deadline - self.now()
            if left <= 0:
                return False, data
            chunk = self.port.read(4096, min(left, 0.1))
            if chunk:
                data += chunk
                if needle in data:
                    return True, data

    def read_quiet(self, quiet_s, timeout_s):
        """Bytes until nothing arrives for quiet_s, or the timeout."""
        data = b""
        deadline = self.now() + timeout_s
        last = self.now()
        while True:
            now = self.now()
            if now >= deadline or now - last >= quiet_s:
                return data
            chunk = self.port.read(4096, min(quiet_s, deadline - now, 0.1))
            if chunk:
                data += chunk
                last = self.now()

    def send(self, line, timeout_s=DEFAULT_TIMEOUT_S, quiet_s=DEFAULT_QUIET_S):
        """One line, CR-terminated; the reply after `  -> ` and the quiet after it."""
        text = console_guard(line, self.disposable)
        self.drain(0.05)
        self.port.write((text + "\r").encode("utf-8"))
        found, data = self.read_until(REPEATER_PROMPT, timeout_s)
        if not found:
            raw = redact(_decode(data))
            self.log("> %s" % redact(text))
            self.log("  (no `  -> ` reply within %.1f s)" % timeout_s)
            return Reply(text, "", True, raw)
        data += self.read_quiet(quiet_s, timeout_s)
        raw = _decode(data)
        reply = raw.split(_decode(REPEATER_PROMPT), 1)[1]
        reply = reply.replace("\r\n", "\n").replace("\r", "\n").rstrip("\n")
        reply = redact(reply)
        self.log("> %s" % redact(text))
        for one in reply.splitlines() or [""]:
            self.log("  -> %s" % one)
        return Reply(text, reply, False, redact(raw))

    def send_reset(self, line, timeout_s=0.5):
        """A line that takes the board away under the port: upstream's own `reboot`.

        The nRF52840 resets as the line is taken and the USB device goes with it, so the open
        handle dies: on Windows the next read of it raises `SerialException ... OSError(22, ...,
        433)` rather than timing out (the first RAK4630 on this bench, p3.4-02; the port is gone
        0.2 s after the line and back 1.4 s later). The line went out and the board is resetting,
        which is what the line means - so the death of the port is the expected answer. The caller
        waits for the application's port to come back."""
        try:
            return self.send(line, timeout_s=timeout_s)
        except (IOError, OSError) as exc:
            self.log("> %s" % line)
            self.log("  (the port went away with the reset, which is the reset: %s)"
                     % str(exc).split(".")[0])
            return Reply(line, "", True, "")

    def read_banner(self, timeout_s=4.0):
        """What a repeater prints when it boots: `Repeater ID: <64 hex>` and any `First boot:`
        lines. Returns a dict with repeater_id (or None), first_boot (a list of lines) and raw."""
        found, data = self.read_until(b"Repeater ID: ", timeout_s)
        if found:
            # the 64 hex digits and the newline after them
            more_found, more = self.read_until(b"\n", 2.0)
            data += more
            data += self.read_quiet(0.3, 1.5)
        raw = redact(_decode(data))
        rid = None
        m = re.search(r"Repeater ID:\s*([0-9A-Fa-f]{64})", raw)
        if m:
            rid = m.group(1).upper()
        first_boot = [ln.strip() for ln in raw.splitlines() if ln.strip().startswith("First boot:")]
        return {"repeater_id": rid, "first_boot": first_boot, "raw": raw}


# ----------------------------------------------------------------------------- the scanner

class LineBatch(object):
    """What a scanner said over a span of time: every line that parsed as JSON, every one that
    did not (kept as text), and the count of the latter."""

    def __init__(self):
        self.objects = []
        self.unparsed = []

    @property
    def unparsed_count(self):
        return len(self.unparsed)

    def take(self, text):
        for line in text.replace("\r\n", "\n").replace("\r", "\n").split("\n"):
            if not line.strip():
                continue
            if line.startswith("{"):
                try:
                    obj = json.loads(line)
                except ValueError:
                    self.unparsed.append(line)
                    continue
                if isinstance(obj, dict):
                    self.objects.append(obj)
                else:
                    self.unparsed.append(line)
            else:
                self.unparsed.append(line)

    def of_type(self, t):
        return [o for o in self.objects if o.get("t") == t]


class ScannerConsole(object):
    """One open port to the scanner's NDJSON console (either of its two ports is the same)."""

    def __init__(self, port, log=None, disposable=False, now=None, sleep=None):
        self.port = port
        self.log = log or (lambda line: None)
        self.disposable = disposable
        import time as _time
        self.now = now or _time.monotonic
        self.sleep = sleep or _time.sleep
        self._pending = b""

    def _read_lines(self, timeout_s):
        """Complete lines that arrived within timeout_s (a partial line waits for the next call)."""
        chunk = self.port.read(4096, timeout_s)
        if chunk:
            self._pending += chunk
        if b"\n" not in self._pending:
            return ""
        complete, self._pending = self._pending.rsplit(b"\n", 1)
        return _decode(complete + b"\n")

    def collect(self, seconds):
        """Everything the scanner says for `seconds`, parsed."""
        batch, _raw = self.collect_raw(seconds)
        return batch

    def collect_raw(self, seconds):
        """The same, with the lines as they arrived beside the parsed batch: a capture kept on
        disk is the device's own bytes and not a re-serialisation of them (a `scope` baseline,
        docs/bench.md section 9, is read back by tools/noisescope/waterfall_plot.py)."""
        batch = LineBatch()
        raw = []
        deadline = self.now() + seconds
        while self.now() < deadline:
            text = self._read_lines(min(0.1, max(0.0, deadline - self.now())))
            if text:
                raw.append(text)
            batch.take(text)
        return batch, "".join(raw)

    def send(self, line, timeout_s=3.0):
        """One command, LF-terminated. Returns (ack or None, LineBatch of everything else seen)."""
        text = console_guard(line, self.disposable)
        self.port.write((text + "\n").encode("utf-8"))
        self.log("> %s" % redact(text))
        word = text.split()[0].lower()
        batch = LineBatch()
        ack = None
        deadline = self.now() + timeout_s
        while self.now() < deadline:
            before = len(batch.objects)
            batch.take(self._read_lines(0.1))
            for obj in batch.objects[before:]:
                if obj.get("t") == "ack" and str(obj.get("cmd", "")).lower() == word:
                    ack = obj
            if ack is not None:
                break
        for obj in batch.objects:
            self.log("  %s" % json.dumps(obj, sort_keys=True)[:200])
        if ack is None:
            self.log("  (no ack for %r within %.1f s)" % (word, timeout_s))
        return ack, batch

    def first(self, t, timeout_s):
        """The first object of type t to arrive, or None."""
        deadline = self.now() + timeout_s
        batch = LineBatch()
        while self.now() < deadline:
            before = len(batch.objects)
            batch.take(self._read_lines(0.1))
            for obj in batch.objects[before:]:
                if obj.get("t") == t:
                    return obj
        return None
