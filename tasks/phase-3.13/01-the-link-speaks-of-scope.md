# 01 — The link speaks of scope

**Depends on:** —
**Spec:** `spec/LINK-PROTOCOL.md` 1.3 (the note at its top; §4; §5.1's *Versions*, *The session's version* and the capability bits; §6's rules; §6.2's `scoped`; §6.5) and `spec/fixtures/link/link-v1.3.json`; `src/common/link/codec/Numbers.h` and `Messages.h`; `src/common/link/session/Session.cpp` (`HELLO` at ~L167, `EV_BOOT` at ~L738) and `Interfaces.h` (`MeshClientAccess`, `Services`, `capsOf`); `src/roles/repeater/meshclient/LinkMeshClient.cpp` (`routeOfJob`, `sent`) and `MeshPort.h` (`Route`)
**Size:** one session, a long one.
**Model:** claude-opus-5

## Goal

Link 1.2 has no word for a scope. A flood the mesh client originates is a `Route` with
`direct` false and nothing else, and the port sends it with no transport code. Link 1.3 says
three things about it: in a session of 1.3 such a flood takes the radio's default scope; the
Walter may ask for one send without it; and the answer to a send says which it was.

This task is all of that on the link's side of `MeshPort`: the codec, the session, the mesh
client, and the port's interface. It is written against the tests' fake. No board implements
the new interface until task 02, so the firmware this task builds answers `HELLO` without the
capability and floods as it does today.

## Deliverables

1. **The codec at 1.3.** `kProtoMinor` 3. `FLOOD_SCOPE` is `0x2C`; `kCapFloodScope = 0x100`. The
   request (`mode : u8`) and its answer (`mode : u8`, `scope : str8`). A send's answer gains
   `scoped : u8` at its end, written when the session is 1.3 or later and left off otherwise.
   `test/test_link_codec` holds the codec against **all four** fixture files, the nineteen cases
   1.3 adds among them.
2. **The session remembers its version.** Today `HELLO` is answered with the highest minor both
   ends know and nothing is kept. 1.3 is the first minor whose rules for other messages depend
   on it: the session's version is the one the last `HELLO`'s answer named, and 1.0 before any
   `HELLO` of this boot. A `HELLO` of 1.2 after one of 1.3 is a session of 1.2 again. `EV_BOOT`
   is what it was, but for the bit in `caps` once a board has it.
3. **`FLOOD_SCOPE` in the session.** It is the capability of an interface in `Services`, present
   or not, as `LocalRequestAccess` is: `capsOf` sets the bit from it and from nothing else.
   - No interface: `ERR_UNSUPPORTED`.
   - A session older than 1.3: `ERR_STATE`.
   - A `mode` that is not 0, 1 or 2: `ERR_RANGE`. No `mode` at all: `ERR_MALFORMED`.
   - Otherwise the answer: the mode the next send will have, and the scope's name as the
     interface gives it, 1 to 30 bytes or empty.
   - It needs no job, it is answered while a reply is awaited, nothing goes on the air and
     `client_tx` does not move.
4. **One send without it.** `mode` 2 is for one send. The next `SEND_*` request the RAK executes
   puts the mode back to 1 whatever that send came to: a flood, a direct send, nothing sent, a
   refusal. So do `JOB_BEGIN` and `JOB_END`, the job the RAK ends by itself after 120 seconds
   among them. A link-level retry of a send is not executed again (§3) and changes nothing.
5. **The port is told, and tells.** `Route` gains what the port has to know of a flood: scoped
   or not. It is scoped when the session is 1.3 or later and the mode is 1, and never otherwise.
   The port's sends say back whether a transport code went on the packet, because `scoped` in the
   answer is what happened and not what was asked: a radio with no default scope answers 0 to a
   flood that was asked scoped.
6. **`scoped` in a send's answer**: 1 for a flood that carried the code; 0 for a direct send, a
   flood without one, and nothing sent. In a session older than 1.3 the byte is not there and the
   answer is byte for byte what it is today.
7. **What says 1.2 today.** The image's descriptor (`src/common/fwdesc`), the manifest a release
   builds, the bench tools that speak the link (`tools/linkcli.py`, `tools/linkloop`), the docs
   that name the three fixture files. Find each and make it true; the bench tools read the new
   byte and can ask `FLOOD_SCOPE`.
8. **The words**, and a line in `release-notes/unreleased/` for this task that says nothing
   changes on the air yet.

## Acceptance criteria

- [ ] All four fixture files pass through the codec in both directions (native test).
- [ ] Before any `HELLO`, and after a `HELLO` of 1.2: a flooded login is a `Route` the port is asked to send unscoped, and every answer's bytes are those of the commit before this task (test, compare the frames).
- [ ] After a `HELLO` of 1.3, with a fake port that holds a scope: a flooded login is asked scoped and answered `scoped` 1. `FLOOD_SCOPE` 2, then the login: asked unscoped, answered 0. The send after it: scoped again (test).
- [ ] `mode` 2 and then a direct send; a send refused `ERR_BUSY`; `JOB_BEGIN`; `JOB_END`; 120 seconds of silence: each puts the mode back, and the flood after it is scoped (tests).
- [ ] `mode` 2, a send, and that send's link-level retry: one packet, and the mode is back once (test).
- [ ] A session of 1.3 and a port with no scope: `FLOOD_SCOPE` answers an empty name, and a flood answers `scoped` 0 (test).
- [ ] `HELLO` 1.3 and then `HELLO` 1.2: unscoped again, and the byte gone (test).
- [ ] `FLOOD_SCOPE` is `ERR_UNSUPPORTED` with no interface, `ERR_STATE` in a session of 1.2, `ERR_RANGE` for `mode` 3, `ERR_MALFORMED` with no payload, and answers a name of 30 bytes whole (tests).
- [ ] `SEND_DISCOVERY` is scoped as the others are; `SEND_TRACE` never is (test).
- [ ] The board's build answers `HELLO` without `0x100`, and `TwinOakMesh::dispatch` is what it was (say how it was checked).
- [ ] `python spec/tools/specsync.py verify spec` passes; nothing under `spec/` was edited.
- [ ] `tools\verify.ps1` green. CI green.

## Out of scope

The board's side: task 02. The Walter's half: its pile 14. Changing upstream. The board: no
session opens a port. A release: Thomas presses the button.
