"""The serial layer, behind an interface with a fake.

Everything that can open a port goes through a Ports object: list(), open(), open_ready(). The real one is
pyserial; the fake (tools/bench/fakes.py) records every open, so a test can prove a refusal
opened nothing (tools/bench/test_bench.py). Nothing else in tools/bench/ imports pyserial.

A board is found by its USB serial number and told apart from its own bootloader by the USB
product id: boards/rak4631.json (vendored from the pin) lists 0x239A:0x8029 and 0x239A:0x802A for
the application and 0x239A:0x0029 and 0x239A:0x002A for the bootloader (a double-tap and the
serial-only DFU of the touch), and the Adafruit core's TinyUSB_Port_GetSerialNumber
(libraries/Adafruit_TinyUSB_Arduino/src/arduino/ports/nrf/Adafruit_TinyUSB_nrf.cpp:96-100 at the
core's pinned commit d541301) builds the serial string from the nRF52840's FICR DEVICEID words.
"""

import time

RAK_VID = 0x239A
# boards/rak4631.json's `hwids`, vendored from the platform at the pin: the application (0x8029,
# and 0x802A for a second USB configuration of it) and the bootloader - 0x0029 after a double-tap,
# where it offers its drive, and **0x002A** when the application reset into the serial-only DFU
# that the 1200-baud touch asks for. The first RAK4630 on this bench came up at 0x0029 after a
# double-tap and at 0x002A after the touch (p3.4-02), which is why both are here.
RAK_APP_PIDS = (0x8029, 0x802A)
RAK_BOOT_PIDS = (0x0029, 0x002A)
RAK_APP_PID = RAK_APP_PIDS[0]
RAK_BOOT_PID = RAK_BOOT_PIDS[0]


class PortInfo(object):
    def __init__(self, device, serial_number=None, vid=None, pid=None, description=""):
        self.device = device
        self.serial_number = serial_number
        self.vid = vid
        self.pid = pid
        self.description = description

    def __repr__(self):
        usb = ("%04X:%04X" % (self.vid, self.pid)
               if self.vid is not None and self.pid is not None else "no usb id")
        return "%s (serial %s, %s)" % (self.device, self.serial_number or "none", usb)

    @property
    def is_bootloader(self):
        return self.vid == RAK_VID and self.pid in RAK_BOOT_PIDS

    @property
    def is_application(self):
        return self.vid == RAK_VID and self.pid in RAK_APP_PIDS


def same_serial(a, b):
    return a is not None and b is not None and a.strip().upper() == b.strip().upper()


class Ports(object):
    """The interface. list() -> [PortInfo]; open(device, baud) -> a Port with read(n, timeout_s),
    write(bytes), close(); sleep(s) and now() so a fake can run without waiting."""

    def list(self):
        raise NotImplementedError

    def open(self, device, baud):
        raise NotImplementedError

    def open_ready(self, device, baud, timeout_s=10.0, poll_s=0.25):
        """open(), retried until the port can be opened or the deadline passes.

        Windows lists a CDC port a moment before it can be opened: the first flash of the first
        RAK4630 on this bench had the application enumerate as COM30 and the open of it fail with
        `FileNotFoundError(2)` (p3.4-02, out/bench/flash.log). Every console open of a port that
        has just come back from a flash or a reset goes through here; the last error is raised, so
        a port that never opens still says what it said."""
        deadline = self.now() + timeout_s
        while True:
            try:
                return self.open(device, baud)
            except (IOError, OSError):
                if self.now() >= deadline:
                    raise
                self.sleep(poll_s)

    def sleep(self, seconds):
        time.sleep(seconds)

    def now(self):
        return time.monotonic()

    # ---- what every caller needs, written once over list()

    def find_board(self, serial, bootloader_serial=None):
        """(application port or None, bootloader port or None) for one board's serial number."""
        app = boot = None
        for info in self.list():
            if same_serial(info.serial_number, serial) and info.is_application:
                app = info
            if same_serial(info.serial_number, bootloader_serial or serial) and info.is_bootloader:
                boot = info
        return app, boot

    def find_serial(self, serial):
        """Any port with this serial number (a USB-UART dongle), or None."""
        for info in self.list():
            if same_serial(info.serial_number, serial):
                return info
        return None

    def wait_until_gone(self, predicate, timeout_s, poll_s=0.2):
        """True once no listed port satisfies predicate, False if the timeout came first.

        A board that reset under a DFU takes its port with it, and the port the DFU failed on is
        listed for a moment after: `flash_image` waits for it to go before it tries again
        (p3.4-02, the scanner's watchdog)."""
        deadline = self.now() + timeout_s
        while True:
            if not any(predicate(info) for info in self.list()):
                return True
            if self.now() >= deadline:
                return False
            self.sleep(poll_s)

    def wait_for(self, predicate, timeout_s, poll_s=0.1):
        """The first PortInfo that satisfies predicate within timeout_s, else None."""
        deadline = self.now() + timeout_s
        while True:
            for info in self.list():
                if predicate(info):
                    return info
            if self.now() >= deadline:
                return None
            self.sleep(poll_s)


class SerialPorts(Ports):
    """pyserial. Imported here and nowhere else, so that the tests and --help need nothing."""

    def __init__(self):
        try:
            import serial  # noqa: F401
            import serial.tools.list_ports  # noqa: F401
        except ImportError:
            raise SystemExit("tools/bench needs pyserial for a real port: python -m pip install pyserial")

    def list(self):
        import serial.tools.list_ports
        out = []
        for p in serial.tools.list_ports.comports():
            out.append(PortInfo(p.device, p.serial_number, p.vid, p.pid, p.description or ""))
        return out

    def open(self, device, baud):
        import serial
        port = serial.Serial(port=device, baudrate=baud, bytesize=8, parity=serial.PARITY_NONE,
                             stopbits=1, timeout=0, rtscts=False, dsrdtr=False)
        return SerialPort(port)


class SerialPort(object):
    def __init__(self, port):
        self._port = port

    def read(self, n, timeout_s):
        self._port.timeout = timeout_s
        return self._port.read(n) or b""

    def write(self, data):
        self._port.write(data)
        self._port.flush()

    def close(self):
        try:
            self._port.close()
        except Exception:
            pass
