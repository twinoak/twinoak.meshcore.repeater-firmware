"""The serial layer, behind an interface with a fake.

Everything that can open a port goes through a Ports object: list(), open(). The real one is
pyserial; the fake (tools/bench/fakes.py) records every open, so a test can prove a refusal
opened nothing (tools/bench/test_bench.py). Nothing else in tools/bench/ imports pyserial.

A board is found by its USB serial number and told apart from its own bootloader by the USB
product id: boards/rak4631.json (vendored from the pin) lists 0x239A:0x8029 for the application
and 0x239A:0x0029 for the bootloader, and the Adafruit core's TinyUSB_Port_GetSerialNumber
(libraries/Adafruit_TinyUSB_Arduino/src/arduino/ports/nrf/Adafruit_TinyUSB_nrf.cpp:96-100 at the
core's pinned commit d541301) builds the serial string from the nRF52840's FICR DEVICEID words.
"""

import time

RAK_VID = 0x239A
RAK_APP_PID = 0x8029
RAK_BOOT_PID = 0x0029


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
        return self.vid == RAK_VID and self.pid == RAK_BOOT_PID

    @property
    def is_application(self):
        return self.vid == RAK_VID and self.pid == RAK_APP_PID


def same_serial(a, b):
    return a is not None and b is not None and a.strip().upper() == b.strip().upper()


class Ports(object):
    """The interface. list() -> [PortInfo]; open(device, baud) -> a Port with read(n, timeout_s),
    write(bytes), close(); sleep(s) and now() so a fake can run without waiting."""

    def list(self):
        raise NotImplementedError

    def open(self, device, baud):
        raise NotImplementedError

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
