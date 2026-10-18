"""One user at a time: a lock per board.

A file under the user's local application data (config.data_dir()/locks/<serial>.lock) holding
the process id, the process's own start time and when the lock was taken. It is held for the
whole of a command, so a session and a person at a terminal cannot flash the same board at
once. A stale lock - its process gone, or its pid reused by a process that started later - is
named and taken over; a live one is refused with EXIT_LOCKED.

The file is created with O_EXCL, so two takers cannot both succeed. Taking over a stale lock is
a remove and a second O_EXCL create, which leaves a window of a few microseconds between two
processes that both found it stale at the same instant; the second create fails and that
process is refused, which is the safe side of the race.
"""

import json
import os
import socket
import sys
import time

from config import data_dir


class Locked(Exception):
    """Somebody else holds the board. The message says who."""


class ProcessProbe(object):
    """Is a process alive, and when did it start? The real one asks the operating system; the
    tests hand in a fake."""

    def is_alive(self, pid):
        if sys.platform == "win32":
            return _win_alive(pid)
        try:
            os.kill(pid, 0)
            return True
        except ProcessLookupError:
            return False
        except PermissionError:
            return True
        except OSError:
            return False

    def started_at(self, pid):
        """The process's start time as a Unix epoch, or None when the platform does not say."""
        if sys.platform == "win32":
            return _win_started_at(pid)
        try:
            with open("/proc/%d/stat" % pid, "r") as fh:
                fields = fh.read().rsplit(")", 1)[1].split()
            ticks = int(fields[19])
            with open("/proc/stat", "r") as fh:
                for line in fh:
                    if line.startswith("btime "):
                        btime = int(line.split()[1])
                        break
                else:
                    return None
            return btime + ticks / float(os.sysconf("SC_CLK_TCK"))
        except Exception:
            return None


# ---- Windows, through ctypes. os.kill(pid, 0) is NOT a liveness check on Windows: it calls
# TerminateProcess (the Python documentation says so), which would kill the other user's
# session. OpenProcess with the query right and GetExitCodeProcess is the harmless question.

_PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
_STILL_ACTIVE = 259


def _win_alive(pid):
    import ctypes
    import ctypes.wintypes as wt
    kernel = ctypes.windll.kernel32
    handle = kernel.OpenProcess(_PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
    if not handle:
        return False
    try:
        code = wt.DWORD()
        if not kernel.GetExitCodeProcess(handle, ctypes.byref(code)):
            return False
        return code.value == _STILL_ACTIVE
    finally:
        kernel.CloseHandle(handle)


def _win_started_at(pid):
    import ctypes
    import ctypes.wintypes as wt
    kernel = ctypes.windll.kernel32
    handle = kernel.OpenProcess(_PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
    if not handle:
        return None
    try:
        creation, exit_, kernel_t, user_t = wt.FILETIME(), wt.FILETIME(), wt.FILETIME(), wt.FILETIME()
        if not kernel.GetProcessTimes(handle, ctypes.byref(creation), ctypes.byref(exit_),
                                      ctypes.byref(kernel_t), ctypes.byref(user_t)):
            return None
        ticks = (creation.dwHighDateTime << 32) | creation.dwLowDateTime
        return ticks / 1e7 - 11644473600.0  # FILETIME epoch (1601) to Unix epoch
    finally:
        kernel.CloseHandle(handle)


def _started_about_the_same(a, b):
    """Two readings of one process's start time, which differ by rounding and nothing else."""
    if a is None or b is None:
        return True
    return abs(a - b) < 2.0


class BoardLock(object):
    """with BoardLock(serial, command): ... - held for the whole of a command."""

    def __init__(self, serial, command, probe=None, home=None, log=None):
        self.serial = serial
        self.command = command
        self.probe = probe or ProcessProbe()
        self.home = home or data_dir()
        self.log = log or (lambda line: None)
        self.path = os.path.join(self.home, "locks", "%s.lock" % serial.upper())

    def _record(self):
        return {
            "pid": os.getpid(),
            "process_started": self.probe.started_at(os.getpid()),
            "taken": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
            "command": self.command,
            "host": socket.gethostname(),
            "serial": self.serial,
        }

    def _read_existing(self):
        try:
            with open(self.path, "r", encoding="utf-8") as fh:
                return json.load(fh)
        except (IOError, OSError, ValueError):
            return {}

    def _is_stale(self, existing):
        pid = existing.get("pid")
        if not isinstance(pid, int):
            return True, "the lock file does not name a process"
        if pid == os.getpid():
            return False, ""
        if not self.probe.is_alive(pid):
            return True, "process %d is gone" % pid
        then = existing.get("process_started")
        now = self.probe.started_at(pid)
        if isinstance(then, (int, float)) and now is not None and not _started_about_the_same(then, now):
            return True, "pid %d was reused by a process that started later" % pid
        return False, ""

    def _try_create(self):
        os.makedirs(os.path.dirname(self.path), exist_ok=True)
        try:
            fd = os.open(self.path, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
        except FileExistsError:
            return False
        with os.fdopen(fd, "w", encoding="utf-8") as fh:
            json.dump(self._record(), fh, indent=2)
            fh.write("\n")
        return True

    def acquire(self):
        if self._try_create():
            return self
        existing = self._read_existing()
        stale, why = self._is_stale(existing)
        if not stale:
            raise Locked(
                "board %s is held by pid %s (%s, since %s, on %s): one user at a time. If that "
                "process is a person's terminal, wait for it; the lock is %s"
                % (self.serial, existing.get("pid"), existing.get("command", "?"),
                   existing.get("taken", "?"), existing.get("host", "?"), self.path))
        self.log("taking over a stale lock on %s: %s (it said pid %s, %s, since %s)"
                 % (self.serial, why, existing.get("pid"), existing.get("command", "?"),
                    existing.get("taken", "?")))
        try:
            os.remove(self.path)
        except OSError:
            pass
        if not self._try_create():
            raise Locked("board %s: somebody else took the lock at the same moment; try again"
                         % self.serial)
        return self

    def release(self):
        try:
            existing = self._read_existing()
            if existing.get("pid") == os.getpid():
                os.remove(self.path)
        except OSError:
            pass

    def __enter__(self):
        return self.acquire()

    def __exit__(self, *exc):
        self.release()
        return False
