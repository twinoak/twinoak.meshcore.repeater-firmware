# 02 — The local request (link 1.2)

**Depends on:** nothing in this phase. 01 and this task touch different files.
**Spec:** `spec/LINK-PROTOCOL.md` §5.9 (new in 1.2), §4, §5.1 (the capability bits, *Versions*), §3 (a retry gets the remembered response); `spec/fixtures/link/link-v1.2.json` (the fourteen cases 1.2 adds; everything before them is 1.1's, unchanged); `spec/SYSTEM.md` S-2 and its *exception* of 7 November. Upstream at the pin: `examples/simple_repeater/MyMesh.h` (`handleRequest` and `reply_data`, both **private**) and `MyMesh.cpp` (`handleRequest`); `src/helpers/ClientACL.h` (`ClientInfo`, `PERM_ACL_ADMIN`). This firmware: `src/common/link/session/Interfaces.h` (`Services`, `capsOf`, and `LppAccess` as the pattern), `Session.cpp` (the `kMsgLppGet` case), `src/common/link/codec/` (`Messages`, `Numbers.h`: `kProtoMinor`, the capability bits), `src/roles/repeater/link/BoardLink.cpp` (where the services are wired), `src/roles/repeater/app/TwinOakMesh.h` (the subclass and its rules), `docs/upstream.md` (*The eight places a bump re-reads*, *What of MeshCore this firmware stands on*, *Tier three*), `docs/link.md`; `test/test_link_codec`, `test/test_link_session`. Read only: `../phase-3.8/FOLLOWUPS.md`, the first item (*status and neighbours through the link*).
**Size:** one session, long.
**Model:** claude-opus-5

## Goal

The first unit's repeater (41 on meshmaster.dk) is read by its own Walter over the box link, never
over the air. Its telemetry comes through. Its status and its neighbour list cannot: the link has no
request that yields them, and the Walter answers those jobs *Failed*. So meshmaster.dk shows the
repeater *stale* for good, and without any of the graphs a status read feeds.

That was 3.8's open follow-up. Thomas settled it on 7 November: a new link request with MeshCore's
own bytes, not the CLI's `stats-*` text parsed. Link 1.2 is that request, `LOCAL_REQUEST` (§5.9). It
answers a status, access-list, neighbours or owner-info request as the node's own handler answers an
admin over the mesh, with nothing on the air. This task makes the repeater answer it.

## Deliverables

1. **The codec and the session at 1.2.**
   - The type `0x0D`, the capability bit `0x80`, `kProtoMinor` 2. HELLO answers the highest minor
     both ends know, so a Walter at 1.1 still gets 1.1.
   - The request and response codecs, held against every case of `link-v1.2.json`. The codec and
     session tests keep running `link-v1.json` and `link-v1.1.json` as they do; 1.2 goes next to
     them with its own expected count.
   - The session's rules, in §5.9's order:
     - `ERR_UNSUPPORTED` without the capability;
     - `ERR_MALFORMED` for an empty payload or more than 32 bytes of data;
     - `ERR_RANGE` for a type not in §5.9's table, and for a request the handler answers with
       nothing;
     - otherwise `OK` and the answer's bytes, written into the response after its status byte, as
       `LPP_GET` does.
   - A new optional service, `LocalRequestAccess`, beside `LppAccess`. Present means the bit in
     `capsOf`. The scanner role does not have one.
2. **The answer is upstream's own.**
   - The repeater's service calls upstream's `MyMesh::handleRequest`:
     - with a `ClientInfo` at `PERM_ACL_ADMIN` that is **not** in the ACL (a local object: nothing
       is added and nothing saved);
     - with the request in a zeroed buffer, so the bytes past `data` read zero (§5.9);
     - and copies the answer out of `reply_data`, after its four tag bytes.
   - Both names are private at the pin. Reach them **without editing upstream** (S-2).
     - The recommended way: C++ does not check access for the names in an explicit instantiation
       ([temp.explicit]), which is the known way to take a pointer to a private member. Put it in
       one file, with a comment that says what it is and why, and a native test of the idiom against
       a stand-in class.
     - It must build in the native environment and for the board. If it does not, write
       `logs/STOPPED-02.md` with the compiler's words and end.
     - Not instead: re-implementing the four answers (the neighbour table and the ACL are private
       too, and a copy would drift at every pin), or `#define private public`.
   - Telemetry, `0x03`, is refused with `ERR_RANGE`. `LPP_GET` is its local form, and two ways to the
     same bytes is one too many.
   - Upstream builds its own replies in the same `reply_data`. Say in the code why a call from the
     link's turn of the main loop cannot meet one half-built, and copy the answer out before
     returning.
3. **`docs/upstream.md`.**
   - The new place a bump re-reads: the two private names and `handleRequest`'s signature, and the
     `RepeaterStats`, neighbours and access-list layouts the fixtures were written from.
   - *Tier three* says what a bump that changes one of those layouts means: a new link minor,
     because the answer is on the wire.
4. **The request to upstream, drafted.** In the report, a short issue text for MeshCore asking for
   `handleRequest` to be `protected` (one word in `MyMesh.h`), and why. Thomas files it; nothing is
   opened from here.
5. **Tests.**
   - The native environment does not build MeshCore. So the session's rules are tested against a
     fake `LocalRequestAccess` through the real session, and the board's build compiles the real
     service. Say so in the report.
   - The cases: status; neighbours with an offset and an order; an access-list request whose
     reserved bytes are not zero (`ERR_RANGE`); telemetry (`ERR_RANGE`); 33 bytes of data
     (`ERR_MALFORMED`); a build without the service (`ERR_UNSUPPORTED`); a retry with the same `seq`
     answered from memory; a job in progress, untouched by a local request.
6. **`docs/link.md`**: the request, the bit, and how a person reads it from the Walter's console
   once the Walter's pile 9 (task 06) has its command.
7. **`../phase-3.8/FOLLOWUPS.md`**: the first item ticked, with the decision and this task.

## Acceptance criteria

- [ ] `link-v1.2.json` passes in the codec and session tests, beside 1.0's and 1.1's; HELLO says 1.2 and the bit only where the service is present.
- [ ] The repeater answers from upstream's own `handleRequest`, with upstream unedited; the idiom is in one file, explained, and tested natively; the board build is green.
- [ ] Every rule of §5.9 has a test; `docs/upstream.md` and `docs/link.md` updated; the upstream issue text is in the report.
- [ ] `tools/verify.ps1` green.

## Out of scope

The Walter (its pile 9, task 06) and meshmaster.dk (its pile 4.27, task 176). The release (03). The
board: the bench check is Thomas's, after 03 and the Walter's v0.1.2.
