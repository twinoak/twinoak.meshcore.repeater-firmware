# 02 — The link codec

**Depends on:** 01.
**Spec:** [`spec/LINK-PROTOCOL.md`](../../spec/LINK-PROTOCOL.md) §2–§8, §11; [`spec/fixtures/link/link-v1.json`](../../spec/fixtures/link/link-v1.json); [`spec/tools/linkref.py`](../../spec/tools/linkref.py); [`docs/ARCHITECTURE.md`](../../docs/ARCHITECTURE.md) §4.
**Size:** one session.
**Model:** claude-opus-5

## Goal

The box link as bytes, and nothing else: the checksum, the framing, packets,
and one encode or decode per message of protocol 1.0 — all of it portable, all
of it held against the spec's fixtures. Nothing here touches a UART or MeshCore;
task 03 puts it on the wire.

## Deliverables

All under `src/link/codec/`, with no Arduino and no MeshCore include, no heap,
and no recursion; every function bounded by the buffer it is given.

- **CRC-16/CCITT-FALSE** and **COBS** encode and decode, for buffers up to a
  packet (244 bytes). A decoder that meets a zero, a block that runs past the
  end, or the `0xFF` code refuses the frame.
- **Packet**: build `type, seq, payload, crc`; parse and check one.
- **`FrameReceiver`**: fed bytes one or many at a time, in any chunking, it
  delivers packets and counts what it drops — bad COBS, short, oversize, bad
  CRC, and an overflow of its 246-byte buffer (dropped when the terminator
  arrives). Empty frames are not errors. The counter saturates at 65535.
- **`FrameWriter`**: a packet to frame bytes, with the leading `0x00` when the
  caller says the line has been idle (LINK-PROTOCOL §2).
- **Messages.** A plain struct per payload and the function this firmware
  needs for it: **decode for every request, encode for every response and
  every event** (§5–§8), including the shared pieces — `str8`, the path, the
  reply record, the observation record. Decoders check lengths and legal
  values and say *malformed* rather than reading past the end; trailing bytes
  they do not know are ignored (§5.1, "Versions"). The numbers — types,
  status codes, roles, capability bits, modes — are one header that reads like
  the tables of §3–§5.
- **A `Reader` and `Writer`** over a byte span for the little-endian scalars,
  so no message function does pointer arithmetic by hand.

`[env:native]`'s `build_src_filter` gains these files.

## Tests (native)

`test/test_link_codec/`, reading `spec/fixtures/link/link-v1.json` at run time
(its path as a define, as the agent firmware passes `FIXTURES_DIR`):

- every `crc16` and `cobs` vector, both directions;
- for every case whose `kind` is `request`: decoding `payload` gives `fields`;
- for every case whose `kind` is `response` or `event`: encoding `fields`
  gives `payload`, and the frame built from `(wire_type, seq, payload)` equals
  `frame`;
- every `invalid` frame is refused and counted (the reason word is the
  reference's and need not match);
- the `stream`: fed whole, and fed one byte at a time, the receiver delivers
  exactly the listed packets and counts the listed number of bad frames;
- a request payload one byte short of each layout is *malformed*, never a read
  out of bounds — run the native tests under AddressSanitizer in the container
  and say so in the report;
- a test that fails when the fixture file gains a case the tests do not cover
  (count the cases per type and compare with what was exercised).

## Acceptance criteria

- [ ] `tools/verify.ps1` green; the board build still links (the codec is compiled for the board too, even though nothing calls it yet) and the report gives its flash cost.
- [ ] Every case of the fixture file is exercised; the coverage test above proves it.
- [ ] No `new`, `malloc`, `String` or `std::string` under `src/link/codec/` (a grep in the rails test).
- [ ] The sources compile under the board's C++ standard and the host's; if those differ, the report says which standard the codec is written to and why.

## Not verified here

Nothing: this task is bytes in, bytes out.

## Out of scope

What a request *does* (03 onward). The UART. The Walter's side of the codec —
the same fixtures are tested there, in its own repository.
