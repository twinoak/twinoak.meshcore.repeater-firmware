# 04 — The rails

**Depends on:** 02, 03.
**Spec:** [`spec/SYSTEM.md`](../../spec/SYSTEM.md) S-12, S-13; [`spec/LINK-PROTOCOL.md`](../../spec/LINK-PROTOCOL.md) §6 (the rules at its head); `README.md`, "Rails that hold in every task".
**Size:** one session.
**Model:** claude-opus-5

## Goal

A repeater is shared infrastructure, and this one now takes orders from another
MCU. This task makes sure a mistake on the Walter cannot turn the unit into a
nuisance on the mesh, gives every rule the project states a test that fails
when it is broken, and gathers what only a bench can check into one walk.

## Deliverables

- **A backstop on the client's sends.** The Walter's agent paces itself to a
  budget the server sets. The RAK enforces a **ceiling that budget can never
  reach in normal use**: at most **6 client sends in any 60 seconds** and at
  least **2 seconds** between two, counted where `client_tx` is counted. Over
  the ceiling a send is refused with `ERR_BUSY` and counted in a new counter
  (`sensor get client.refused`). The limits are build constants with their
  reasoning in a comment — the server's default is 2 a minute, 20 seconds
  apart — and the limiter is portable with tests. It does not replace
  upstream's duty-cycle accounting, which already covers every packet the
  radio sends; say in the report how the two relate, from the source at the
  pin (`dutycycle` / `af`, and whether locally originated packets count).
- **Repeating comes first.** Establish from upstream's dispatcher at the pin
  how a locally originated packet is queued against packets waiting to be
  forwarded, and whether a client send can delay a forward by more than its
  own airtime. Write what you find in `docs/ARCHITECTURE.md` §7; if it can
  starve forwarding, propose the smallest fix that does not touch upstream
  and stop short of implementing a queue of our own.
- **The rails test, complete** (`test/test_rails/`): one test, one line per
  rule, each of which fails with a sentence that names the rule:
  1. no `MESH_PACKET_LOGGING`, no `MESH_DEBUG`, in any environment;
  2. no source under `src/` prints or logs a packet payload, a CLI line or its
     reply, an advert's application data, or a password;
  3. no board environment defines `ADMIN_PASSWORD` as `password`, and the
     first-boot path that replaces it exists;
  4. every flood the client originates passes a path-hash size of 2 or more
     (the constant is 2 and nothing passes a literal 1);
  5. `onPeerPathRecv` for the client's peer returns false (no reciprocal
     path);
  6. nothing under `src/` writes a file whose content came from the link's
     `SEND_LOGIN` or a sensitive `SEND_CLI` (the modules they pass through
     include no file-system header);
  7. upstream's INA3221 support is not in the build;
  8. no file under `spec/` differs from its manifest, and no file under the
     pinned MeshCore checkout is referenced for writing by any script;
  9. no heap allocation in `src/link/` or `src/meshclient/`.
  Where a rule can only be approximated by a grep, the test says what it does
  and does not catch.
- **`docs/link.md`, finished**: every request this firmware answers, its
  capability bit, an example exchange from `linkcli.py`, the settings table,
  the LPP channel table, the attention reasons, the two ceilings.
- **`docs/bench.md`, in order.** Collect every *Not verified here* list of
  phases 1 and 2 into one walk in the order the hardware repository's
  `BENCH-BREADBOARD.md` §7 brings the rig up — the repeater alone on USB; the
  link from a laptop; the clock; management; power; the mesh client;
  observations — each line a thing to do and what to expect, with a box to
  tick and room for a number. Lines that need the production boards rather
  than the breadboard rig are marked so.
- **`README.md`**: "State" says what the firmware does now, and "Rails" names
  the test that holds each.

## Tests (native)

- The limiter against a fake clock: six in a minute, the seventh refused; the
  two-second gap; the window sliding; a refused send does not count.
- The rails test passes — and for each of its nine lines, the report shows the
  deliberate change that made it fail, then reverted.

## Acceptance criteria

- [ ] `tools/verify.ps1` green.
- [ ] The report answers the two questions about upstream's duty cycle and transmit queue with the lines it read.
- [ ] Each rail failed once on purpose and the sentence it printed is quoted.
- [ ] `docs/bench.md` is one ordered walk and nothing from an earlier report's bench list is missing from it.

## Not verified here (Thomas)

The walk itself.

## Out of scope

New behaviour. Anything on the Walter.
