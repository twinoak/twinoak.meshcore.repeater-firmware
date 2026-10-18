#!/usr/bin/env python3
"""bench - the one way a session touches a board.

    python tools/bench/bench.py status                     the bench as described, and what is there
    python tools/bench/bench.py check <image.zip>          every guard on an image; no port is opened
    python tools/bench/bench.py record [--image <zip>]     what is on the board now (begins a run)
    python tools/bench/bench.py flash <image.zip> [--found <zip>]
    python tools/bench/bench.py console "ver"              one console line, the reply
    python tools/bench/bench.py console --scanner "info"   the scanner's NDJSON console
    python tools/bench/bench.py console --scanner --collect 60
    python tools/bench/bench.py restore                    put back what was found

The rig is bench.local.json at the repository root (bench.example.json is the shape, config.py
says what each field means). The rails are in the tool, not in the caller's intentions:

  * no config, or no port with the RAK's serial number: every command exits 3 (no bench) and says
    why. Not a failure - the walk goes to "Not verified here";
  * flash takes one kind of file, the DFU package the build or the release made, and refuses
    everything images.py lists - before any port is opened (exit 2);
  * a repeater image is never flashed with the RF port off the dummy load, and a repeater found on
    a board whose RF port is not the dummy load is never replaced (it could not be put back);
  * one user at a time: a lock per board, held for the whole command (exit 4 when held);
  * the console refuses to send a line that sets a password or the private key, `erase`, and the
    bootloader lines, and never prints `password now:` (console.py);
  * before the first flash of a run the board is recorded, and `restore` puts that role back and
    checks the Repeater ID; a run that cannot be put back says so in capitals (exit 1).

Exit codes: 0 done; 1 the board did not do what was asked; 2 refused by a guard, nothing opened;
3 no bench; 4 the board is held by another process.

Run it with PlatformIO's own Python (~/.platformio/penv), which has pyserial; adafruit-nrfutil is
the copy PlatformIO bundles (flash.py). No SWD, no UF2, no erase, no `reboot 2`/`3`, ever.
"""

import argparse
import os
import sys
import traceback

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

import config as cfg  # noqa: E402
import images  # noqa: E402
from console import RepeaterConsole, ScannerConsole, console_guard  # noqa: E402
from flash import (DOUBLE_TAP, FlashError, SubprocessRunner, Timeouts, flash_image, locate_nrfutil,  # noqa: E402
                   log_flash, read_board, read_repeater, read_scanner)
from lock import BoardLock, Locked  # noqa: E402
from runs import RestoreNotPossible, choose_restore_image, clear_run, load_run, record_board  # noqa: E402

REPO = os.path.abspath(os.path.join(HERE, "..", ".."))


class Bench(object):
    """Every command, with the layers behind it injectable: tests hand in fakes."""

    def __init__(self, repo=REPO, ports=None, runner=None, home=None, out=None, probe=None,
                 nrfutil=None, python=None, timeouts=None, region_for=None, commit=None):
        self.repo = repo
        self._ports = ports
        self.runner = runner or SubprocessRunner()
        self.home = home or cfg.data_dir()
        self.out = out or sys.stdout
        self.probe = probe
        self._nrfutil = nrfutil
        self.python = python or sys.executable
        self.timeouts = timeouts or Timeouts()
        self.region_for = region_for or (lambda path: images.find_region(path, self.repo))
        self.commit = commit
        self.config = None

    # ---- plumbing

    def say(self, line):
        self.out.write(line + "\n")
        self.out.flush()

    @property
    def ports(self):
        if self._ports is None:
            from ports import SerialPorts
            self._ports = SerialPorts()
        return self._ports

    @property
    def nrfutil(self):
        if self._nrfutil is None:
            self._nrfutil = locate_nrfutil()
        return self._nrfutil

    def load(self):
        self.config = cfg.load_config(self.repo)
        return self.config

    def require_board(self):
        """The board's ports, or NoBench."""
        app, boot = self.ports.find_board(self.config.serial, self.config.bootloader_serial)
        if app is None and boot is None:
            raise cfg.NoBench("no port with serial number %s is present: no bench (the tool never "
                              "picks a port by any other rule)" % self.config.serial)
        return app, boot

    def lock(self, command):
        return BoardLock(self.config.serial, command, probe=self.probe, home=self.home, log=self.say)

    def inspect(self, path):
        return images.inspect_image(path, self.config.board, self.config.rf, self.region_for(path))

    # ---- commands

    def cmd_status(self, args):
        self.load()
        c = self.config
        self.say("bench: %s (%s)" % (c.source, c.board))
        self.say("  serial     %s" % c.serial)
        self.say("  rf         %s" % c.rf)
        self.say("  identity   %s" % c.identity)
        self.say("  dongle     %s" % (c.dongle_serial or "none"))
        app, boot = self.ports.find_board(c.serial, c.bootloader_serial)
        self.say("  board      %s" % (app and ("application on %s" % app.device) or
                                      boot and ("bootloader on %s" % boot.device) or "NOT PRESENT"))
        if c.dongle_serial:
            dongle = self.ports.find_serial(c.dongle_serial)
            self.say("  link       %s" % (dongle and ("dongle on %s" % dongle.device) or "dongle not present"))
        run = load_run(self.home, c.serial)
        self.say("  run        %s" % (run.describe() if run else "none open"))
        lock_path = BoardLock(c.serial, "status", home=self.home).path
        self.say("  lock       %s" % ("held (%s)" % lock_path if os.path.exists(lock_path) else "free"))
        if app is None and boot is None:
            raise cfg.NoBench("no port with serial number %s is present: no bench" % c.serial)
        return cfg.EXIT_OK

    def cmd_check(self, args):
        self.load()
        image = self.inspect(args.image)
        self.say("ok: %s" % image.summary())
        self.say("    rf %s: a %s image %s" % (self.config.rf, image.role,
                                               "transmits" if image.transmits else "never transmits"))
        return cfg.EXIT_OK

    def cmd_record(self, args):
        self.load()
        if args.image:
            self.inspect(args.image)  # the image said to be on the board must pass the guards too
        self.require_board()
        with self.lock("record"):
            record = record_board(self.config, self.ports, self.runner, self.say, self.home,
                                  found_image=os.path.abspath(args.image) if args.image else None,
                                  timeouts=self.timeouts, python=self.python, repo=self.repo)
        if record.role is None:
            self.say("RECORDED NOTHING RESTORABLE: %s" % record.describe())
            return cfg.EXIT_FAILED
        self.say("recorded %s" % record.describe())
        return cfg.EXIT_OK

    def _flash_and_confirm(self, image, label):
        """The flash and the reading afterwards; returns what the board said."""
        try:
            outcome = flash_image(image, self.config, self.ports, self.runner, self.say, self.nrfutil,
                                  python=self.python, timeouts=self.timeouts)
        except FlashError as exc:
            log_flash(self.home, self.repo, self.config, image, None, "failed: %s" % str(exc).split(".")[0],
                      extra={"what": label})
            raise
        if image.role == "scanner":
            said = read_scanner(self.ports, outcome.application_port, self.say, self.config.disposable,
                                self.timeouts, expect_banner=True)
        else:
            said = read_repeater(self.ports, outcome.application_port, self.say, self.config.disposable,
                                 self.timeouts, expect_banner=True, reboot_for_id=True)
        outcome.board_says = said
        result = "ok" if said.get("version") == image.version and said.get("role") == image.role else "mismatch"
        log_flash(self.home, self.repo, self.config, image, outcome, result,
                  extra={"board_says": dict((k, v) for k, v in said.items() if k != "info"), "what": label})
        self.say("flashed %s; the board says %s %s%s" % (
            image.summary(), said.get("role"), said.get("version"),
            (", Repeater ID %s" % said["repeater_id"]) if said.get("repeater_id") else ""))
        if result != "ok":
            raise FlashError("the board does not say what the image said: image %s %s, board %s %s"
                             % (image.role, image.version, said.get("role"), said.get("version")))
        return said

    def cmd_flash(self, args):
        self.load()
        image = self.inspect(args.image)              # every guard, before any port
        found_image = None
        if args.found:
            found_image = os.path.abspath(args.found)
            self.inspect(found_image)
        self.require_board()
        with self.lock("flash %s" % os.path.basename(args.image)):
            run = load_run(self.home, self.config.serial)
            if run is None:
                self.say("no run is open for %s: recording what is on the board first" % self.config.serial)
                run = record_board(self.config, self.ports, self.runner, self.say, self.home,
                                   found_image=found_image, timeouts=self.timeouts, python=self.python,
                                   repo=self.repo)
            elif found_image and not run.found_image:
                run.found_image = found_image
                from runs import save_run
                save_run(self.home, run)
            # The board must be restorable afterwards, or it is not touched: a repeater found on an
            # RF port that is not the dummy load could not be flashed back by this tool.
            if run.role is not None and images.role_transmits(run.role) and self.config.rf != "dummy-load":
                raise cfg.Refused("the board carries a %s image and the RF port is %r: flashing %s would "
                                  "leave a board this tool may not restore (a %s image is flashed only "
                                  "on the dummy load). Nothing was touched."
                                  % (run.role, self.config.rf, os.path.basename(args.image), run.role))
            self._flash_and_confirm(image, "flash")
        return cfg.EXIT_OK

    def cmd_restore(self, args):
        self.load()
        self.require_board()                          # no board is no bench, before anything else
        run = load_run(self.home, self.config.serial)
        try:
            path, how = choose_restore_image(run, self.config.board, self.repo)
        except RestoreNotPossible as exc:
            self.say("RESTORE NOT POSSIBLE: %s" % exc)
            return cfg.EXIT_FAILED
        image = self.inspect(path)
        with self.lock("restore"):
            if run.version and image.version != run.version and run.found_image:
                self.say("note: the run recorded version %s and the image given says %s" % (run.version, image.version))
            try:
                said = self._flash_and_confirm(image, "restore")
            except FlashError as exc:
                self.say("RESTORE FAILED: %s" % exc)
                return cfg.EXIT_FAILED
            checks = []
            if run.role == "repeater":
                if run.repeater_id and said.get("repeater_id") == run.repeater_id:
                    checks.append("Repeater ID %s matches the one recorded" % run.repeater_id)
                elif run.repeater_id:
                    self.say("RESTORE FAILED: THE REPEATER ID IS %s AND THE RUN RECORDED %s - THE NODE'S "
                             "IDENTITY IS NOT WHAT IT WAS" % (said.get("repeater_id"), run.repeater_id))
                    return cfg.EXIT_FAILED
                else:
                    checks.append("no Repeater ID was recorded, so none could be checked")
                if run.name and said.get("name") and said["name"] != run.name:
                    self.say("RESTORE FAILED: THE NAME IS %r AND THE RUN RECORDED %r" % (said["name"], run.name))
                    return cfg.EXIT_FAILED
            if run.found_image:
                which = "the recorded version %s, from %s" % (image.version, how)
            else:
                which = "this run's own image of that role (%s %s, %s)" % (image.role, image.version, how.split(" (")[-1].rstrip(")"))
            clear_run(self.home, self.config.serial)
            self.say("restored: %s back on the board - %s; %s" % (run.role, which, "; ".join(checks) or "ok"))
        return cfg.EXIT_OK

    def cmd_console(self, args):
        self.load()
        if not args.collect:
            console_guard(args.line, self.config.disposable)  # refused before any port is opened
        app, boot = self.require_board()
        if app is None:
            raise cfg.Refused("the board is in its bootloader on %s: there is no console to talk to "
                              "(`bench flash` an image, or a double-tap and a flash)" % boot.device)
        with self.lock("console"):
            port = self.ports.open(app.device, 115200)
            try:
                if args.scanner:
                    console = ScannerConsole(port, log=(lambda s: None), disposable=self.config.disposable,
                                             now=self.ports.now, sleep=self.ports.sleep)
                    if args.collect:
                        batch = console.collect(float(args.collect))
                        for obj in batch.objects:
                            self.say(_json(obj))
                        for text in batch.unparsed:
                            self.say("# not JSON: %s" % text)
                        self.say("# %d JSON lines, %d not JSON" % (len(batch.objects), batch.unparsed_count))
                        return cfg.EXIT_OK if batch.unparsed_count == 0 else cfg.EXIT_FAILED
                    ack, batch = console.send(args.line, timeout_s=args.timeout)
                    for obj in batch.objects:
                        self.say(_json(obj))
                    for text in batch.unparsed:
                        self.say("# not JSON: %s" % text)
                    return cfg.EXIT_OK if (ack is None or ack.get("ok", True)) else cfg.EXIT_FAILED
                console = RepeaterConsole(port, log=(lambda s: None), disposable=self.config.disposable,
                                          now=self.ports.now, sleep=self.ports.sleep)
                console.drain(0.2)
                reply = console.send(args.line, timeout_s=args.timeout)
                if reply.timed_out:
                    self.say("(no reply within %.1f s)" % args.timeout)
                    return cfg.EXIT_FAILED
                self.say(reply.text)
                return cfg.EXIT_OK
            finally:
                port.close()


def _json(obj):
    import json
    return json.dumps(obj, sort_keys=True)


def build_parser():
    p = argparse.ArgumentParser(prog="bench", description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--repo", default=REPO, help="the repository root (default: this checkout)")
    subs = p.add_subparsers(dest="command", metavar="<command>")
    subs.add_parser("status", help="the bench as described, and what is present").set_defaults(run="cmd_status")
    check = subs.add_parser("check", help="every guard on an image; opens no port")
    check.add_argument("image")
    check.set_defaults(run="cmd_check")
    record = subs.add_parser("record", help="record what is on the board (begins a run)")
    record.add_argument("--image", default=None, help="the DFU package that is on the board now, if known")
    record.set_defaults(run="cmd_record")
    flash = subs.add_parser("flash", help="flash one of our DFU packages")
    flash.add_argument("image")
    flash.add_argument("--found", default=None, help="the DFU package that is on the board now, if known")
    flash.set_defaults(run="cmd_flash")
    subs.add_parser("restore", help="put back what the run found").set_defaults(run="cmd_restore")
    console = subs.add_parser("console", help="one console line, and the reply")
    console.add_argument("line", nargs="?", default=None)
    console.add_argument("--scanner", action="store_true", help="the scanner's NDJSON console")
    console.add_argument("--collect", default=None, metavar="SECONDS", help="(scanner) collect lines for this long")
    console.add_argument("--timeout", type=float, default=2.5)
    console.set_defaults(run="cmd_console")
    return p


def main(argv=None, bench=None):
    parser = build_parser()
    args = parser.parse_args(argv)
    if not args.command:
        parser.print_help()
        return cfg.EXIT_REFUSED
    if args.command == "console" and not args.collect and not args.line:
        parser.error("console needs a line, or --scanner --collect SECONDS")
    bench = bench or Bench(repo=args.repo)
    try:
        return getattr(bench, args.run)(args)
    except cfg.NoBench as exc:
        bench.say("no bench: %s" % exc)
        return cfg.EXIT_NO_BENCH
    except cfg.Refused as exc:
        bench.say("refused: %s" % exc)
        return cfg.EXIT_REFUSED
    except Locked as exc:
        bench.say("locked: %s" % exc)
        return cfg.EXIT_LOCKED
    except FlashError as exc:
        bench.say("failed: %s" % exc)
        return cfg.EXIT_FAILED
    except (IOError, OSError) as exc:
        bench.say("failed: %s. %s" % (exc, DOUBLE_TAP))
        return cfg.EXIT_FAILED
    except Exception:
        bench.say("failed: " + traceback.format_exc().rstrip())
        return cfg.EXIT_FAILED


if __name__ == "__main__":
    sys.exit(main())
