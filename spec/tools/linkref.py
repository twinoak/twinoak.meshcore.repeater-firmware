#!/usr/bin/env python3
"""Reference implementation of the TwinOak box link, protocol 1.0.

The link is the UART between the LoRa box (RAK4630) and the LTE box (Walter); LINK-PROTOCOL.md
is the specification and this file is the same thing as code. It is the source of the fixtures
both firmware repositories test against:

    python3 linkref.py gen   ../fixtures/link/link-v1.json     write the fixtures
    python3 linkref.py check ../fixtures/link/link-v1.json     re-derive every case and compare
    python3 linkref.py decode 0003010203...00                  print what one frame says

Standard library only. Nothing here talks to a serial port: it is a codec, on purpose.
"""

import json
import struct
import sys

PROTO_MAJOR = 1
PROTO_MINOR = 0
MAX_PAYLOAD = 240          # bytes of payload in one packet
MAX_PACKET = MAX_PAYLOAD + 4  # type + seq + payload + crc16

# ----------------------------------------------------------------------------- framing


def crc16(data: bytes) -> int:
    """CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF, no reflection, no final xor."""
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def cobs_encode(raw: bytes) -> bytes:
    """Consistent Overhead Byte Stuffing. The result holds no 0x00."""
    out = bytearray()
    block = bytearray()
    for b in raw:
        if b == 0:
            out.append(len(block) + 1)
            out += block
            block = bytearray()
        else:
            block.append(b)
            if len(block) == 254:
                out.append(0xFF)
                out += block
                block = bytearray()
    out.append(len(block) + 1)
    out += block
    return bytes(out)


def cobs_decode(enc: bytes) -> bytes:
    """Inverse of cobs_encode. Raises ValueError on a malformed block."""
    if not enc:
        raise ValueError("empty")
    out = bytearray()
    i = 0
    while i < len(enc):
        code = enc[i]
        if code == 0:
            raise ValueError("zero inside a frame")
        i += 1
        end = i + code - 1
        if end > len(enc):
            raise ValueError("block runs past the end")
        out += enc[i:end]
        i = end
        if code != 0xFF and i < len(enc):
            out.append(0)
    return bytes(out)


def make_packet(msg_type: int, seq: int, payload: bytes) -> bytes:
    if len(payload) > MAX_PAYLOAD:
        raise ValueError("payload over %d bytes" % MAX_PAYLOAD)
    body = bytes([msg_type & 0xFF, seq & 0xFF]) + payload
    return body + struct.pack("<H", crc16(body))


def make_frame(msg_type: int, seq: int, payload: bytes) -> bytes:
    """One packet on the wire: its COBS encoding and the 0x00 that ends it."""
    return cobs_encode(make_packet(msg_type, seq, payload)) + b"\x00"


def parse_frame(frame: bytes):
    """frame is everything up to and including the terminating 0x00. Returns (type, seq, payload)."""
    if not frame or frame[-1] != 0:
        raise ValueError("no terminator")
    packet = cobs_decode(frame[:-1])
    if len(packet) < 4:
        raise ValueError("short")
    if len(packet) > MAX_PACKET:
        raise ValueError("oversize")
    body, crc = packet[:-2], struct.unpack("<H", packet[-2:])[0]
    if crc16(body) != crc:
        raise ValueError("crc")
    return body[0], body[1], body[2:]


# ----------------------------------------------------------------------------- the numbers

# Requests (Walter -> RAK in 1.0). A response is the request's type with bit 7 set.
HELLO, PING, STATUS, TIME_SET, TIME_GET, REBOOT, IDENTITY, SLEEP_NOTICE = 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08
CLI, POWER_GET, ENV_GET, LPP_GET = 0x09, 0x0A, 0x0B, 0x0C
JOB_BEGIN, JOB_END, PATH_SET, PATH_GET = 0x20, 0x21, 0x22, 0x23
SEND_LOGIN, SEND_REQUEST, SEND_CLI, SEND_DISCOVERY, SEND_TRACE = 0x24, 0x25, 0x26, 0x27, 0x28
REPLY_GET, OBS_PULL, MESH_COUNTERS = 0x29, 0x2A, 0x2B
# Events (RAK -> Walter), never answered.
EV_BOOT, EV_MESH_REPLY, EV_ATTENTION = 0x60, 0x61, 0x62

RESPONSE_BIT = 0x80

NAMES = {
    HELLO: "HELLO", PING: "PING", STATUS: "STATUS", TIME_SET: "TIME_SET", TIME_GET: "TIME_GET",
    REBOOT: "REBOOT", IDENTITY: "IDENTITY", SLEEP_NOTICE: "SLEEP_NOTICE", CLI: "CLI",
    POWER_GET: "POWER_GET", ENV_GET: "ENV_GET", LPP_GET: "LPP_GET",
    JOB_BEGIN: "JOB_BEGIN", JOB_END: "JOB_END", PATH_SET: "PATH_SET", PATH_GET: "PATH_GET",
    SEND_LOGIN: "SEND_LOGIN", SEND_REQUEST: "SEND_REQUEST", SEND_CLI: "SEND_CLI",
    SEND_DISCOVERY: "SEND_DISCOVERY", SEND_TRACE: "SEND_TRACE", REPLY_GET: "REPLY_GET",
    OBS_PULL: "OBS_PULL", MESH_COUNTERS: "MESH_COUNTERS",
    EV_BOOT: "EV_BOOT", EV_MESH_REPLY: "EV_MESH_REPLY", EV_ATTENTION: "EV_ATTENTION",
}

# status, the first byte of every response
OK, ERR_UNSUPPORTED, ERR_MALFORMED, ERR_BUSY, ERR_STATE, ERR_RANGE, ERR_FAILED, ERR_DENIED = range(8)

# HELLO: role
ROLE_UNKNOWN, ROLE_REPEATER, ROLE_BRIDGE, ROLE_SCANNER = 0, 1, 2, 3
# HELLO: caps
CAP_CLI, CAP_POWER, CAP_ENV, CAP_MESH_CLIENT, CAP_OBSERVATIONS, CAP_ATTENTION, CAP_TIME_PERSIST = (1 << n for n in range(7))

# path_mode
PATH_NONE, PATH_DIRECT, PATH_HOPS = 0, 1, 2
# reply record: state
REPLY_NONE, REPLY_PENDING, REPLY_TIMEOUT, REPLY_RESPONSE, REPLY_TEXT, REPLY_TRACE = range(6)

# ----------------------------------------------------------------------------- field codecs


class Reader:
    def __init__(self, data: bytes):
        self.d, self.i = data, 0

    def take(self, n):
        if self.i + n > len(self.d):
            raise ValueError("payload too short")
        v = self.d[self.i:self.i + n]
        self.i += n
        return v

    def rest(self):
        v = self.d[self.i:]
        self.i = len(self.d)
        return v

    def u8(self): return self.take(1)[0]
    def i8(self): return struct.unpack("<b", self.take(1))[0]
    def u16(self): return struct.unpack("<H", self.take(2))[0]
    def i16(self): return struct.unpack("<h", self.take(2))[0]
    def u32(self): return struct.unpack("<I", self.take(4))[0]
    def str8(self): return self.take(self.u8()).decode("utf-8")


def u8(v): return struct.pack("<B", v)
def i8(v): return struct.pack("<b", v)
def u16(v): return struct.pack("<H", v)
def i16(v): return struct.pack("<h", v)
def u32(v): return struct.pack("<I", v)


def str8(s):
    b = s.encode("utf-8")
    if len(b) > 255:
        raise ValueError("string over 255 bytes")
    return u8(len(b)) + b


def enc_path(f):
    mode = f["path_mode"]
    if mode != PATH_HOPS:
        return u8(mode) + u8(0) + u8(0)
    hashes = bytes.fromhex(f["hashes"])
    size, count = f["hash_size"], f["hop_count"]
    if size not in (1, 2, 3) or not 1 <= count <= 63 or len(hashes) != size * count or len(hashes) > 64:
        raise ValueError("path")
    return u8(mode) + u8(size) + u8(count) + hashes


def dec_path(r, f):
    f["path_mode"], size, count = r.u8(), r.u8(), r.u8()
    if f["path_mode"] == PATH_HOPS:
        f["hash_size"], f["hop_count"] = size, count
        f["hashes"] = r.take(size * count).hex()


def enc_hello_body(f):
    return (u8(f["proto_major"]) + u8(f["proto_minor"]) + u8(f["role"]) + u8(f["slot"]) + u8(f["max_payload"])
            + u32(f["caps"]) + u32(f["boot_id"]) + str8(f["fw_version"]) + str8(f["board"]) + str8(f["upstream"]))


def dec_hello_body(r, f):
    for k in ("proto_major", "proto_minor", "role", "slot", "max_payload"):
        f[k] = r.u8()
    f["caps"], f["boot_id"] = r.u32(), r.u32()
    f["fw_version"], f["board"], f["upstream"] = r.str8(), r.str8(), r.str8()


def enc_reply(f):
    out = u8(f["state"]) + u32(f["tag"]) + u8(f["via_path"]) + i8(f["snr_q4"]) + i16(f["rssi"]) + u32(f["elapsed_ms"])
    st = f["state"]
    if st == REPLY_RESPONSE:
        out += bytes.fromhex(f["data"])
    elif st == REPLY_TEXT:
        out += f["text"].encode("utf-8")
    elif st == REPLY_TRACE:
        hashes, snrs = bytes.fromhex(f["hashes"]), bytes.fromhex(f["snrs"])
        out += u8(f["trace_flags"]) + i8(f["final_snr_q4"]) + u8(len(hashes)) + hashes + u8(len(snrs)) + snrs
    return out


def dec_reply(r, f):
    f["state"], f["tag"], f["via_path"] = r.u8(), r.u32(), r.u8()
    f["snr_q4"], f["rssi"], f["elapsed_ms"] = r.i8(), r.i16(), r.u32()
    st = f["state"]
    if st == REPLY_RESPONSE:
        f["data"] = r.rest().hex()
    elif st == REPLY_TEXT:
        f["text"] = r.rest().decode("utf-8")
    elif st == REPLY_TRACE:
        f["trace_flags"], f["final_snr_q4"] = r.u8(), r.i8()
        f["hashes"] = r.take(r.u8()).hex()
        f["snrs"] = r.take(r.u8()).hex()


def enc_obs(o):
    path = bytes.fromhex(o["path"])
    plen = o["path_len"]
    if len(path) != ((plen >> 6) + 1) * (plen & 63):
        raise ValueError("observation path")
    app = bytes.fromhex(o["app_data"])
    return (bytes.fromhex(o["pubkey"]) + u32(o["adv_ts"]) + u32(o["age_s"]) + i8(o["snr_q4"]) + i16(o["rssi"])
            + u8(plen) + path + u8(len(app)) + app)


def dec_obs(r):
    o = {"pubkey": r.take(32).hex(), "adv_ts": r.u32(), "age_s": r.u32(), "snr_q4": r.i8(), "rssi": r.i16()}
    o["path_len"] = r.u8()
    o["path"] = r.take(((o["path_len"] >> 6) + 1) * (o["path_len"] & 63)).hex()
    o["app_data"] = r.take(r.u8()).hex()
    return o


def encode(kind: str, msg_type: int, f: dict) -> bytes:
    """kind is 'request', 'response' or 'event'; f is the field dictionary of LINK-PROTOCOL.md."""
    t = msg_type
    if kind == "response":
        out = u8(f["status"])
        if f["status"] != OK:
            return out
        if t == HELLO: return out + enc_hello_body(f)
        if t == PING: return out + u32(f["nonce"]) + u32(f["uptime_s"])
        if t == STATUS:
            return (out + u32(f["uptime_s"]) + u8(f["reset_reason"]) + u8(f["time_state"]) + u32(f["epoch"])
                    + u16(f["flags"]) + u8(f["obs_pending"]) + u16(f["obs_dropped"]) + u16(f["link_errors"])
                    + u8(f["attention"]))
        if t == TIME_SET: return out + u32(f["prev_epoch"]) + u32(f["epoch"]) + u8(f["time_state"])
        if t == TIME_GET: return out + u32(f["epoch"]) + u8(f["time_state"])
        if t in (REBOOT, SLEEP_NOTICE, JOB_BEGIN, JOB_END, PATH_SET): return out
        if t == IDENTITY: return out + bytes.fromhex(f["pubkey"]) + str8(f["name"])
        if t == CLI: return out + f["text"].encode("utf-8")
        if t == POWER_GET:
            out += u8(f["valid"])
            for ch in f["channels"]:
                out += u16(ch["bus_mv"]) + i16(ch["current_ma"])
            return out
        if t == ENV_GET: return out + u8(f["valid"]) + i16(f["temp_centi"]) + u16(f["rh_centi"]) + u32(f["pressure_pa"])
        if t == LPP_GET: return out + bytes.fromhex(f["lpp"])
        if t == PATH_GET: return out + enc_path(f)
        if t in (SEND_LOGIN, SEND_REQUEST, SEND_CLI, SEND_DISCOVERY, SEND_TRACE):
            return out + u8(f["send"]) + u32(f["tag"]) + u32(f["sender_ts"])
        if t == REPLY_GET: return out + enc_reply(f)
        if t == OBS_PULL:
            out += u8(f["remaining"]) + u16(f["dropped"]) + u8(len(f["records"]))
            for o in f["records"]:
                out += enc_obs(o)
            return out
        if t == MESH_COUNTERS:
            return (out + u32(f["rx"]) + u32(f["tx"]) + u32(f["client_tx"]) + u32(f["adverts"])
                    + u32(f["adverts_dropped"]) + u32(f["obs_dropped"]) + i16(f["noise_floor"])
                    + i16(f["last_rssi"]) + i8(f["last_snr_q4"]))
    elif kind == "request":
        if t == HELLO:
            return (u8(f["proto_major"]) + u8(f["proto_minor"]) + u8(f["peer_kind"]) + u8(f["max_payload"])
                    + str8(f["fw_version"]))
        if t == PING: return u32(f["nonce"])
        if t in (STATUS, TIME_GET, IDENTITY, POWER_GET, ENV_GET, JOB_END, PATH_GET, REPLY_GET, MESH_COUNTERS): return b""
        if t == TIME_SET: return u32(f["epoch"]) + u8(f["source"]) + u8(f["flags"])
        if t == REBOOT: return u8(f["mode"]) + u16(f["delay_ms"])
        if t == SLEEP_NOTICE: return u32(f["expect_back_s"])
        if t == CLI: return f["text"].encode("utf-8")
        if t == LPP_GET: return u8(f["perm_mask"])
        if t == JOB_BEGIN: return bytes.fromhex(f["pubkey"]) + enc_path(f)
        if t == PATH_SET: return enc_path(f)
        if t == SEND_LOGIN: return u32(f["timeout_ms"]) + bytes.fromhex(f["password"])
        if t == SEND_REQUEST: return u32(f["timeout_ms"]) + u8(f["req_type"]) + bytes.fromhex(f["data"])
        if t == SEND_CLI: return u32(f["timeout_ms"]) + u8(f["flags"]) + f["text"].encode("utf-8")
        if t == SEND_DISCOVERY: return u32(f["timeout_ms"])
        if t == SEND_TRACE: return u32(f["timeout_ms"]) + u8(f["hash_size"]) + bytes.fromhex(f["hashes"])
        if t == OBS_PULL: return u8(f["max"])
    elif kind == "event":
        if t == EV_BOOT: return u8(f["reset_reason"]) + enc_hello_body(f)
        if t == EV_MESH_REPLY: return enc_reply(f)
        if t == EV_ATTENTION: return u8(f["reason"]) + u16(f["detail"]) + u8(f["repeat"])
    raise ValueError("no layout for %s 0x%02x" % (kind, t))


def decode(kind: str, msg_type: int, payload: bytes) -> dict:
    r, f, t = Reader(payload), {}, msg_type
    if kind == "response":
        f["status"] = r.u8()
        if f["status"] != OK:
            return f
        if t == HELLO: dec_hello_body(r, f)
        elif t == PING: f["nonce"], f["uptime_s"] = r.u32(), r.u32()
        elif t == STATUS:
            f["uptime_s"], f["reset_reason"], f["time_state"], f["epoch"] = r.u32(), r.u8(), r.u8(), r.u32()
            f["flags"], f["obs_pending"], f["obs_dropped"] = r.u16(), r.u8(), r.u16()
            f["link_errors"], f["attention"] = r.u16(), r.u8()
        elif t == TIME_SET: f["prev_epoch"], f["epoch"], f["time_state"] = r.u32(), r.u32(), r.u8()
        elif t == TIME_GET: f["epoch"], f["time_state"] = r.u32(), r.u8()
        elif t == IDENTITY: f["pubkey"], f["name"] = r.take(32).hex(), r.str8()
        elif t == CLI: f["text"] = r.rest().decode("utf-8")
        elif t == POWER_GET:
            f["valid"] = r.u8()
            f["channels"] = [{"bus_mv": r.u16(), "current_ma": r.i16()} for _ in range(3)]
        elif t == ENV_GET: f["valid"], f["temp_centi"], f["rh_centi"], f["pressure_pa"] = r.u8(), r.i16(), r.u16(), r.u32()
        elif t == LPP_GET: f["lpp"] = r.rest().hex()
        elif t == PATH_GET: dec_path(r, f)
        elif t in (SEND_LOGIN, SEND_REQUEST, SEND_CLI, SEND_DISCOVERY, SEND_TRACE):
            f["send"], f["tag"], f["sender_ts"] = r.u8(), r.u32(), r.u32()
        elif t == REPLY_GET: dec_reply(r, f)
        elif t == OBS_PULL:
            f["remaining"], f["dropped"] = r.u8(), r.u16()
            f["records"] = [dec_obs(r) for _ in range(r.u8())]
        elif t == MESH_COUNTERS:
            for k in ("rx", "tx", "client_tx", "adverts", "adverts_dropped", "obs_dropped"):
                f[k] = r.u32()
            f["noise_floor"], f["last_rssi"], f["last_snr_q4"] = r.i16(), r.i16(), r.i8()
    elif kind == "request":
        if t == HELLO:
            f["proto_major"], f["proto_minor"], f["peer_kind"], f["max_payload"] = r.u8(), r.u8(), r.u8(), r.u8()
            f["fw_version"] = r.str8()
        elif t == PING: f["nonce"] = r.u32()
        elif t == TIME_SET: f["epoch"], f["source"], f["flags"] = r.u32(), r.u8(), r.u8()
        elif t == REBOOT: f["mode"], f["delay_ms"] = r.u8(), r.u16()
        elif t == SLEEP_NOTICE: f["expect_back_s"] = r.u32()
        elif t == CLI: f["text"] = r.rest().decode("utf-8")
        elif t == LPP_GET: f["perm_mask"] = r.u8()
        elif t == JOB_BEGIN:
            f["pubkey"] = r.take(32).hex()
            dec_path(r, f)
        elif t == PATH_SET: dec_path(r, f)
        elif t == SEND_LOGIN: f["timeout_ms"], f["password"] = r.u32(), r.rest().hex()
        elif t == SEND_REQUEST: f["timeout_ms"], f["req_type"], f["data"] = r.u32(), r.u8(), r.rest().hex()
        elif t == SEND_CLI: f["timeout_ms"], f["flags"], f["text"] = r.u32(), r.u8(), r.rest().decode("utf-8")
        elif t == SEND_DISCOVERY: f["timeout_ms"] = r.u32()
        elif t == SEND_TRACE: f["timeout_ms"], f["hash_size"], f["hashes"] = r.u32(), r.u8(), r.rest().hex()
        elif t == OBS_PULL: f["max"] = r.u8()
    elif kind == "event":
        if t == EV_BOOT:
            f["reset_reason"] = r.u8()
            dec_hello_body(r, f)
        elif t == EV_MESH_REPLY: dec_reply(r, f)
        elif t == EV_ATTENTION: f["reason"], f["detail"], f["repeat"] = r.u8(), r.u16(), r.u8()
    return f


# ----------------------------------------------------------------------------- the fixtures



def advert_app(adv_type, name, lat=None, lon=None):
    """An advert's app_data as MeshCore's AdvertDataBuilder writes it: a flags byte (the type in the
    low nibble, 0x10 = a position follows, 0x80 = a name follows), the position as two int32 of
    degrees times 1e6, then the name. At most 32 bytes."""
    flags = adv_type | (0x10 if lat is not None else 0) | (0x80 if name else 0)
    out = u8(flags)
    if lat is not None:
        out += struct.pack("<ii", round(lat * 1e6), round(lon * 1e6))
    out += name.encode("utf-8")
    if len(out) > 32:
        raise ValueError("app_data over 32 bytes")
    return out.hex()


KEY_A = "a1b2c3d4e5f60718293a4b5c6d7e8f90a1b2c3d4e5f60718293a4b5c6d7e8f90"
KEY_B = "7f3a9c02e1b44d9a0f6c2b7d8e9a1c3f5d7b9e0a2c4e6f8a0b1c2d3e4f5a6b7c"
HELLO_FIELDS = {
    "proto_major": 1, "proto_minor": 0, "role": ROLE_REPEATER, "slot": 0, "max_payload": 240,
    "caps": CAP_CLI | CAP_POWER | CAP_ENV | CAP_MESH_CLIENT | CAP_OBSERVATIONS | CAP_ATTENTION | CAP_TIME_PERSIST,
    "boot_id": 0x5EED1234, "fw_version": "0.1.0+abc1234", "board": "rak4630-single",
    "upstream": "repeater-v1.17.1+d9296435",
}

CASES = [
    # name, direction, kind, type, seq, fields
    ("hello request", "w2r", "request", HELLO, 1,
     {"proto_major": 1, "proto_minor": 0, "peer_kind": 1, "max_payload": 240, "fw_version": "0.1.0+def5678"}),
    ("hello response", "r2w", "response", HELLO, 1, dict(status=OK, **HELLO_FIELDS)),
    ("hello refused: major not spoken", "r2w", "response", HELLO, 2, {"status": ERR_UNSUPPORTED}),
    ("ping request", "w2r", "request", PING, 3, {"nonce": 0xDEADBEEF}),
    ("ping response", "r2w", "response", PING, 3, {"status": OK, "nonce": 0xDEADBEEF, "uptime_s": 86400}),
    ("status request", "w2r", "request", STATUS, 4, {}),
    ("status response", "r2w", "response", STATUS, 4,
     {"status": OK, "uptime_s": 3600, "reset_reason": 2, "time_state": 2, "epoch": 1791720000, "flags": 0x0010,
      "obs_pending": 7, "obs_dropped": 0, "link_errors": 1, "attention": 0}),
    ("time set", "w2r", "request", TIME_SET, 5, {"epoch": 1791720000, "source": 1, "flags": 0}),
    ("time set response", "r2w", "response", TIME_SET, 5,
     {"status": OK, "prev_epoch": 1715770400, "epoch": 1791720000, "time_state": 2}),
    ("time set refused: backwards without force", "r2w", "response", TIME_SET, 6, {"status": ERR_RANGE}),
    ("time get", "w2r", "request", TIME_GET, 7, {}),
    ("time get response", "r2w", "response", TIME_GET, 7, {"status": OK, "epoch": 1791720042, "time_state": 2}),
    ("reboot into the uf2 bootloader", "w2r", "request", REBOOT, 8, {"mode": 1, "delay_ms": 500}),
    ("reboot response", "r2w", "response", REBOOT, 8, {"status": OK}),
    ("identity request", "w2r", "request", IDENTITY, 9, {}),
    ("identity response", "r2w", "response", IDENTITY, 9, {"status": OK, "pubkey": KEY_B, "name": "TwinOak Risskov"}),
    ("sleep notice", "w2r", "request", SLEEP_NOTICE, 10, {"expect_back_s": 900}),
    ("sleep notice response", "r2w", "response", SLEEP_NOTICE, 10, {"status": OK}),
    ("cli request", "w2r", "request", CLI, 11, {"text": "get radio"}),
    ("cli response", "r2w", "response", CLI, 11, {"status": OK, "text": "> 869.618,62.5,8,8"}),
    ("cli response with a zero-length reply", "r2w", "response", CLI, 12, {"status": OK, "text": ""}),
    ("power request", "w2r", "request", POWER_GET, 13, {}),
    ("power response", "r2w", "response", POWER_GET, 13,
     {"status": OK, "valid": 7, "channels": [{"bus_mv": 18100, "current_ma": 120},
                                             {"bus_mv": 4012, "current_ma": -52},
                                             {"bus_mv": 3980, "current_ma": 85}]}),
    ("env request", "w2r", "request", ENV_GET, 14, {}),
    ("env response, below freezing", "r2w", "response", ENV_GET, 14,
     {"status": OK, "valid": 7, "temp_centi": -350, "rh_centi": 8125, "pressure_pa": 101325}),
    ("lpp request", "w2r", "request", LPP_GET, 15, {"perm_mask": 0x05}),
    ("lpp response", "r2w", "response", LPP_GET, 15, {"status": OK, "lpp": "0174019001670028"}),
    ("job begin, flood", "w2r", "request", JOB_BEGIN, 16, {"pubkey": KEY_A, "path_mode": PATH_NONE}),
    ("job begin, direct neighbour", "w2r", "request", JOB_BEGIN, 17, {"pubkey": KEY_A, "path_mode": PATH_DIRECT}),
    ("job begin, two hops of two bytes", "w2r", "request", JOB_BEGIN, 18,
     {"pubkey": KEY_A, "path_mode": PATH_HOPS, "hash_size": 2, "hop_count": 2, "hashes": "a1b27f3a"}),
    ("job begin response", "r2w", "response", JOB_BEGIN, 18, {"status": OK}),
    ("job end", "w2r", "request", JOB_END, 19, {}),
    ("job end response", "r2w", "response", JOB_END, 19, {"status": OK}),
    ("path set: forget", "w2r", "request", PATH_SET, 20, {"path_mode": PATH_NONE}),
    ("path get", "w2r", "request", PATH_GET, 21, {}),
    ("path get response, learned from a reply", "r2w", "response", PATH_GET, 21,
     {"status": OK, "path_mode": PATH_HOPS, "hash_size": 2, "hop_count": 1, "hashes": "7f3a"}),
    ("login with no password (the acl login)", "w2r", "request", SEND_LOGIN, 22, {"timeout_ms": 12000, "password": ""}),
    ("login with a password", "w2r", "request", SEND_LOGIN, 23,
     {"timeout_ms": 12000, "password": "example-pw".encode().hex()}),
    ("login sent as a flood", "r2w", "response", SEND_LOGIN, 23,
     {"status": OK, "send": 1, "tag": 0, "sender_ts": 1791720050}),
    ("send refused: no job", "r2w", "response", SEND_LOGIN, 24, {"status": ERR_STATE}),
    ("send refused: a reply is still awaited", "r2w", "response", SEND_REQUEST, 25, {"status": ERR_BUSY}),
    ("status request to the peer", "w2r", "request", SEND_REQUEST, 26, {"timeout_ms": 8000, "req_type": 0x01, "data": ""}),
    ("neighbours request to the peer", "w2r", "request", SEND_REQUEST, 27,
     {"timeout_ms": 8000, "req_type": 0x06, "data": "00090000080011223344"}),
    ("request sent direct", "r2w", "response", SEND_REQUEST, 27,
     {"status": OK, "send": 2, "tag": 1791720051, "sender_ts": 1791720051}),
    ("cli line to the peer", "w2r", "request", SEND_CLI, 28, {"timeout_ms": 8000, "flags": 0, "text": "get name"}),
    ("cli line made from a password", "w2r", "request", SEND_CLI, 29,
     {"timeout_ms": 8000, "flags": 1, "text": "password example-new"}),
    ("cli line sent", "r2w", "response", SEND_CLI, 29, {"status": OK, "send": 2, "tag": 0, "sender_ts": 1791720052}),
    ("discovery", "w2r", "request", SEND_DISCOVERY, 30, {"timeout_ms": 20000}),
    ("discovery sent", "r2w", "response", SEND_DISCOVERY, 30,
     {"status": OK, "send": 1, "tag": 1791720053, "sender_ts": 1791720053}),
    ("trace over a two-byte round trip", "w2r", "request", SEND_TRACE, 31,
     {"timeout_ms": 16000, "hash_size": 2, "hashes": "7f3aa1b27f3a46f6"}),
    ("trace refused: three-byte hashes", "r2w", "response", SEND_TRACE, 32, {"status": ERR_RANGE}),
    ("reply get", "w2r", "request", REPLY_GET, 33, {}),
    ("reply get: still waiting", "r2w", "response", REPLY_GET, 33,
     {"status": OK, "state": REPLY_PENDING, "tag": 1791720051, "via_path": 0, "snr_q4": 0, "rssi": 0x7FFF,
      "elapsed_ms": 1200}),
    ("reply get: nothing held", "r2w", "response", REPLY_GET, 34,
     {"status": OK, "state": REPLY_NONE, "tag": 0, "via_path": 0, "snr_q4": 0, "rssi": 0x7FFF, "elapsed_ms": 0}),
    ("observations pull", "w2r", "request", OBS_PULL, 35, {"max": 4}),
    ("observations: two adverts", "r2w", "response", OBS_PULL, 35,
     {"status": OK, "remaining": 5, "dropped": 0, "records": [
         {"pubkey": KEY_A, "adv_ts": 1791719000, "age_s": 41, "snr_q4": 29, "rssi": -84, "path_len": 0x00, "path": "",
          "app_data": advert_app(2, "Grenaa #01", 56.413, 10.878)},
         {"pubkey": KEY_B, "adv_ts": 1791718000, "age_s": 300, "snr_q4": -10, "rssi": 0x7FFF, "path_len": 0x42,
          "path": "a1b27f3a", "app_data": advert_app(1, "Window")},
     ]}),
    ("observations: none waiting", "r2w", "response", OBS_PULL, 36,
     {"status": OK, "remaining": 0, "dropped": 3, "records": []}),
    ("mesh counters", "w2r", "request", MESH_COUNTERS, 37, {}),
    ("mesh counters response", "r2w", "response", MESH_COUNTERS, 37,
     {"status": OK, "rx": 12345, "tx": 6789, "client_tx": 42, "adverts": 230, "adverts_dropped": 1, "obs_dropped": 3,
      "noise_floor": -112, "last_rssi": -97, "last_snr_q4": 25}),
    ("unknown request type answered", "r2w", "response", 0x5E, 38, {"status": ERR_UNSUPPORTED}),
    ("boot event", "r2w", "event", EV_BOOT, 0, dict(reset_reason=3, **HELLO_FIELDS)),
    ("mesh reply: a response that came back as a path", "r2w", "event", EV_MESH_REPLY, 1,
     {"state": REPLY_RESPONSE, "tag": 1791720051, "via_path": 1, "snr_q4": 25, "rssi": -97, "elapsed_ms": 4210,
      "data": "c40f0000a4ff9dff39300000851a0000d2040000"}),
    ("mesh reply: a cli reply", "r2w", "event", EV_MESH_REPLY, 2,
     {"state": REPLY_TEXT, "tag": 0, "via_path": 0, "snr_q4": 31, "rssi": -80, "elapsed_ms": 2890, "text": "> TwinOak Grenaa"}),
    ("mesh reply: a trace", "r2w", "event", EV_MESH_REPLY, 3,
     {"state": REPLY_TRACE, "tag": 1791720054, "via_path": 0, "snr_q4": 22, "rssi": -101, "elapsed_ms": 6100,
      "trace_flags": 1, "final_snr_q4": 22, "hashes": "7f3aa1b27f3a46f6", "snrs": "1d f6 14 16".replace(" ", "")}),
    ("mesh reply: silence", "r2w", "event", EV_MESH_REPLY, 4,
     {"state": REPLY_TIMEOUT, "tag": 1791720055, "via_path": 0, "snr_q4": 0, "rssi": 0x7FFF, "elapsed_ms": 8000}),
    ("attention: the battery is low", "r2w", "event", EV_ATTENTION, 5, {"reason": 2, "detail": 3450, "repeat": 0}),
    ("attention: an admin asked for the walter, third time", "r2w", "event", EV_ATTENTION, 7,
     {"reason": 4, "detail": 0, "repeat": 2}),
    ("a payload with zeros in it (cobs at work)", "w2r", "request", SEND_REQUEST, 0,
     {"timeout_ms": 0, "req_type": 0, "data": "0000ff00"}),
    ("the largest payload", "r2w", "response", LPP_GET, 255, {"status": OK, "lpp": "5a" * 239}),
]


def reject_reason(frame: bytes) -> str:
    """Why parse_frame refuses a frame: 'cobs', 'short', 'oversize' or 'crc'. Informative - a receiver
    has to refuse the frame and count it; it does not have to agree on the word."""
    try:
        parse_frame(frame)
    except ValueError as e:
        return str(e) if str(e) in ("short", "oversize", "crc") else "cobs"
    return ""


def invalid_frames():
    good = make_frame(PING, 3, u32(0xDEADBEEF))
    packet = make_packet(PING, 3, u32(0xDEADBEEF))
    flipped = bytearray(packet)
    flipped[3] ^= 0x01
    over = bytes([PING, 1]) + b"\x11" * (MAX_PAYLOAD + 1)
    over += struct.pack("<H", crc16(over))
    return [
        ("a flipped bit in the payload", cobs_encode(bytes(flipped)) + b"\x00"),
        ("the crc bytes swapped", cobs_encode(packet[:-2] + packet[-1:] + packet[-2:-1]) + b"\x00"),
        ("three bytes: no room for a crc", cobs_encode(b"\x02\x03\x04") + b"\x00"),
        ("a cobs block that runs past the end", bytes([0x09, 0x02, 0x03]) + b"\x00"),
        ("one byte too many of payload", cobs_encode(over) + b"\x00"),
        ("a good frame cut short by a stray terminator", good[:4] + b"\x00"),
    ]


def build_fixtures():
    cases = []
    for name, direction, kind, t, seq, fields in CASES:
        payload = encode(kind, t, fields)
        wire_type = t | RESPONSE_BIT if kind == "response" else t
        cases.append({
            "name": name, "dir": direction, "kind": kind, "type": t, "type_name": NAMES.get(t, "?"),
            "wire_type": wire_type, "seq": seq, "fields": fields,
            "payload": payload.hex(), "packet": make_packet(wire_type, seq, payload).hex(),
            "frame": make_frame(wire_type, seq, payload).hex(),
        })
    # Nothing here is longer than a packet may be (MAX_PACKET), so no run reaches 254 bytes and the
    # 0xFF code of general COBS never appears: LINK-PROTOCOL.md section 2.
    cobs_vectors = [b"\x00", b"\x00\x00", b"\x11\x22\x00\x33", b"\x11\x22\x33\x44", b"\x11\x00\x00\x00",
                    bytes(range(1, MAX_PACKET + 1)), b"\x00" + bytes(range(1, MAX_PACKET)),
                    b"\x01" * (MAX_PACKET - 1) + b"\x00"]
    crc_vectors = [b"123456789", b"", b"\x00", b"\x02\x03\xef\xbe\xad\xde"]
    stream = b"\x00\x00" + make_frame(PING, 3, u32(1)) + b"garbage\x00" + b"\x00" + make_frame(STATUS, 4, b"")
    return {
        "link": "%d.%d" % (PROTO_MAJOR, PROTO_MINOR),
        "about": "Generated by software/spec/tools/linkref.py from LINK-PROTOCOL.md. Never edited by hand.",
        "max_payload": MAX_PAYLOAD,
        "crc16": [{"data": d.hex(), "crc": "%04x" % crc16(d)} for d in crc_vectors],
        "cobs": [{"raw": d.hex(), "encoded": cobs_encode(d).hex()} for d in cobs_vectors],
        "cases": cases,
        "invalid": [{"name": n, "frame": f.hex(), "why": reject_reason(f)} for n, f in invalid_frames()],
        "stream": {
            "about": "A receiver fed these bytes delivers exactly these packets, in order, and counts one bad frame. "
                     "Empty frames (two terminators in a row) are not errors.",
            "bytes": stream.hex(),
            "packets": [{"wire_type": PING, "seq": 3, "payload": u32(1).hex()},
                        {"wire_type": STATUS, "seq": 4, "payload": ""}],
            "bad_frames": 1,
        },
    }


def check(path):
    with open(path, "r", encoding="utf-8") as fh:
        fx = json.load(fh)
    n = 0
    for v in fx["crc16"]:
        assert "%04x" % crc16(bytes.fromhex(v["data"])) == v["crc"], v
        n += 1
    for v in fx["cobs"]:
        raw, enc = bytes.fromhex(v["raw"]), bytes.fromhex(v["encoded"])
        assert cobs_encode(raw) == enc and 0 not in enc and 0xFF not in (enc[0],), v
        assert cobs_decode(enc) == raw, v
        n += 1
    for c in fx["cases"]:
        payload = bytes.fromhex(c["payload"])
        assert encode(c["kind"], c["type"], c["fields"]) == payload, c["name"]
        assert decode(c["kind"], c["type"], payload) == c["fields"], (c["name"], decode(c["kind"], c["type"], payload))
        t, seq, got = parse_frame(bytes.fromhex(c["frame"]))
        assert (t, seq, got) == (c["wire_type"], c["seq"], payload), c["name"]
        assert len(payload) <= fx["max_payload"], c["name"]
        n += 1
    for v in fx["invalid"]:
        why = reject_reason(bytes.fromhex(v["frame"]))
        assert why and why == v["why"], (v["name"], why)
        n += 1
    packets, bad, buf = [], 0, bytearray()
    for b in bytes.fromhex(fx["stream"]["bytes"]):
        if b != 0:
            buf.append(b)
            continue
        if buf:
            try:
                t, seq, p = parse_frame(bytes(buf) + b"\x00")
                packets.append({"wire_type": t, "seq": seq, "payload": p.hex()})
            except ValueError:
                bad += 1
        buf = bytearray()
    assert packets == fx["stream"]["packets"] and bad == fx["stream"]["bad_frames"]
    return n + 1


def main(argv):
    if len(argv) == 3 and argv[1] == "gen":
        with open(argv[2], "w", encoding="utf-8", newline="\n") as fh:
            json.dump(build_fixtures(), fh, indent=1, ensure_ascii=True)
            fh.write("\n")
        print("wrote %s: %d checks pass" % (argv[2], check(argv[2])))
        return 0
    if len(argv) == 3 and argv[1] == "check":
        print("%s: %d checks pass" % (argv[2], check(argv[2])))
        return 0
    if len(argv) == 3 and argv[1] == "decode":
        t, seq, payload = parse_frame(bytes.fromhex(argv[2]))
        print("type 0x%02x %s%s seq %d payload %s" % (t, NAMES.get(t & 0x7F if t >= 0x80 else t, "?"),
                                                      " (response)" if t & 0x80 else "", seq, payload.hex() or "-"))
        return 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
