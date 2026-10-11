# 04 — The clock

**Depends on:** 03.
**Spec:** [`spec/LINK-PROTOCOL.md`](../../spec/LINK-PROTOCOL.md) §5.3, §5.4; [`docs/ARCHITECTURE.md`](../../docs/ARCHITECTURE.md) §5; the hardware repository's `PROJECT-KNOWLEDGE.md` §1 at `b798aab043281fa4c9b17802a164fa94be12e206` (the RTC is not fitted in batch 1).
**Size:** one session.
**Model:** claude-opus-5

## Goal

A repeater whose clock never runs backwards, on a board with no RTC. MeshCore
judges replays by timestamps: a node that restarts into upstream's fixed date
in 2024 sends adverts and replies that everyone who heard its earlier ones
drops. The Walter will set the time after every restart — and this firmware
must behave before it does, and when there is no Walter at all.

## Read first, in the pinned checkout

`src/helpers/ArduinoHelpers.h` (`VolatileRTCClock`), `AutoDiscoverRTCClock.*`,
`mesh::RTCClock` (`getCurrentTimeUnique`), where `main.cpp` sends the boot
advert, and `CommonCLI.cpp`'s `time` and `clock sync` — including its refusal
to go backwards.

## Deliverables

- **`ClockKeeper`** (`src/clock/`, the rules portable):
  - **Two memories.** *RAM that survives a reset*: the last known time and a
    checksum, in memory the C runtime does not initialise — valid after a
    watchdog, pin or software reset, gone after a power cut. Establish where
    such memory can safely live on this board (the bootloader and the
    SoftDevice use RAM too; the bootloader's own double-reset word is the
    evidence that some survives) and cite what you found; if no place is safe,
    say so and ship the file alone. *A file* in the node's file system: the
    last known time, rewritten at most once an hour and on a restart the
    firmware itself asks for. Flash wear is the reason for the hour: put the
    arithmetic in a comment.
  - **At boot**, in order: a hardware RTC that answers with a sane time wins
    (`time_state` 3). Otherwise the surviving RAM value (the clock simply
    continues; state 1). Otherwise the file's value **plus the save
    interval** (state 1): the node cannot know how long it was down, and being
    an hour ahead is harmless where being a second behind its own last advert
    is not. Otherwise upstream's default (state 0).
  - **From the link**: `TIME_SET` and `TIME_GET` as §5.4 says. Forward always.
    Backwards only when *force* is set, or when the state is 0 or 1 **and no
    timestamp has left this node since boot** (the advert hold below is what
    makes that common); otherwise `ERR_RANGE`. A set makes the state 2 and
    saves both memories.
  - It sets upstream's clock through `rtc_clock.setCurrentTime` and nothing
    else of upstream's; the console's own `time` and `clock sync` keep working
    and move the keeper's state to 2 when they succeed (poll the clock for a
    jump rather than patching the CLI).
- **The boot advert waits.** Our `main.cpp` sends it, so ours decides when:
  as soon as the state is 2 or 3, or after **60 seconds** without that. The
  Walter normally sets the clock within a second or two of `EV_BOOT`. `STATUS`
  flag bit 3 is set while the hold is on. Upstream's periodic adverts are its
  own; say in the report when the first of those can fire after boot at the
  default interval, and that it is later than the hold.
- **`HELLO`'s `TIME_PERSIST` capability**, `STATUS`'s `time_state` and `epoch`
  and bit 3, and the setting `clock.state` (ARCHITECTURE §4).
- **`linkcli.py`**: `time` (get), `time set [epoch]` (the laptop's clock by
  default), `time set --force <epoch>`.

## Tests (native)

`test/test_clock/` — the rules as a pure function of (RTC answer, RAM value,
file value, now):

- every boot order above, including a corrupt RAM value and a corrupt file;
- the file is written at most once an hour however often time is set, and
  always on an asked-for restart;
- forward set; backward set refused in state 2; allowed in state 1 before
  anything was sent; refused in state 1 after; allowed with *force*;
- a restored clock is never earlier than the saved one plus the interval;
- the hold: released by a set, released by the timeout, never held in state 3;
- the `TIME_SET` fixture cases, through the session, byte for byte.

## Acceptance criteria

- [ ] `tools/verify.ps1` green.
- [ ] The report says where the surviving RAM lives and why that place is safe — or that none was found — citing the linker script and the bootloader's memory layout it read.
- [ ] The report gives the flash-wear arithmetic for the file.
- [ ] No upstream file is touched to hold the advert or to learn of a console `time`.

## Not verified here (Thomas, with a board)

Add to `docs/bench.md`, "3. The clock":

- `linkcli.py time set`, then `reboot 0`: after the restart `time` is a few
  seconds on, state *restored*, not 2024. The same after pressing the reset
  switch, and after a watchdog reset if one can be provoked.
- Power off for a minute and on again: the time is the saved time plus an
  hour or less, state *restored*; a `time set` to the real time is accepted.
- With no laptop attached the boot advert appears on a nearby companion after
  about a minute; with `time set` sent within a second of `EV_BOOT`, at once.
- A nearby companion that saw this repeater's adverts before a power cycle
  still accepts its adverts after it.

## Out of scope

Writing the RV-3028's backup-switchover configuration (a later task, when an
RTC is fitted). The Walter's side of setting the time.
