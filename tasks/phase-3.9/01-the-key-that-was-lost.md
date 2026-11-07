# 01 — The key that was lost

**Depends on:** nothing in this phase.
**Spec:** `../phase-3.8/FOLLOWUPS.md`: *The first radio did not keep its key* and the review of 7 November (*A radio left half-written*: the journal's step 0, the box powered, step 3's wording, bootloader 0.4.4); `docs/flashing.md` §3a; this firmware's identity and settings:
- where MeshCore keeps the node's key at the pin (`IdentityStore`, the file's name and path) and what makes a new one;
- `InternalFS` / Adafruit LittleFS: where it lives, and what `begin()` does with a volume it cannot mount;
- `src/roles/repeater/app/FirstBoot.cpp` (what a first boot decides, and from what marker) and `firstboot/Profile.cpp`;
- the descriptor's application region, `boards/base/rak4630/` (bootloader 0.4.4: the bootloader settings page, what it does with an invalid application, the UF2 handler's families).

The Walter firmware, **read only, at its tag `v0.1.0`** (`git show v0.1.0:<path>`, the code that did it):
- `components/core/flasher/flasher.cpp` (the order of the pages, the first page last, the settings page);
- `components/core/update/radio_update.cpp` (what a resume after a restart re-does);
- its `tasks/phase-8/FOLLOWUPS.md` (the journal's `check` entry with a zero hash; the review's S4).

The commits between the image that made the key (the `0.0.0-dev` of `main` that Thomas flashed by DFU in October, after the radio's base on 22 October) and `v0.1.0` (2577a00): `git log` over `src/` for identity, file system, first boot.
**Size:** one session, long.
**Model:** claude-opus-5

## Goal

Find out what made the first radio a new node between the watchdog loop of 6 November and its first
boot after the `.uf2`. Fix it if the cause is in this firmware. Rewrite the guide so it does not
promise what did not happen.

## Deliverables

1. **The cause, from the code.** Read each candidate and rule it in or out with evidence: a line, a
   constant, a commit.
   - **A half-written application ran.** Did the Walter's 0.1.0 flasher write the first page (the
     vector table) last, every time, including on a resume after a restart? What does bootloader
     0.4.4 do when the settings page says *valid* and the first page is erased, or is not?
   - **The file system was reformatted.** Does `InternalFS.begin()`, or this firmware, format a volume
     it cannot mount? What could have left it unmountable: a write cut by one of the resets the
     Walter's boots put on nRESET?
   - **A first boot regenerated the key.** Does `FirstBoot` or MeshCore make a new identity when a
     marker is missing, even though a key file exists? Did the key's path or format change between the
     `0.0.0-dev` that made it and v0.1.0?
   - **A page outside the image was erased.** The application region against the file system's
     pages; the flasher's region; the UF2 families.

   Name the cause, or the two candidates the code cannot tell apart, with the bench line that would.
2. **A fix, where the cause is here.** If this firmware can lose the key, for example by formatting a
   volume it fails to mount, make it keep the key, or refuse in a way a person sees, rather than
   silently becoming a new node. Make it deterministic, and test it natively. If the cause is the
   Walter's or the bootloader's, write the follow-up for that repository, in its `FOLLOWUPS.md`'s
   form, in the report.
3. **`docs/flashing.md` §3a rewritten.**
   - **Step 0:** `rak update status` on the Walter first. An unfinished *flashing-candidate* is
     resumed at the Walter's next boot and would flash over a radio recovered by hand. And the box
     powered: on the production adapter USB-C does not power the RAK.
   - **Step 3:** say what the Walter's install and this release's `.uf2` (family 0xADA52840) can
     write, and no more.
   - **Step 1:** what bootloader 0.4.4 does with an invalid application, checked in 0.4.4 and not
     0.4.3, or say what was checked.
   - **The key:** a recovery may give the radio a new identity, as the first one did. How to tell:
     `rak hello` and `agent id` on the Walter, or the unit's companion key on meshmaster.dk.
   - **What then:**
     - confirm the key on meshmaster.dk;
     - register the unit's repeater again from it;
     - re-apply the settings a person had: name, position, admin and guest passwords, and anything
       not in the profile, through the Walter's `rak cli`.

     Say which settings the profile restores by itself.
4. **3.8's review items** are ticked or moved here.

## Acceptance criteria

- [ ] Each candidate is ruled in or out with evidence; the cause named, or the bench line that will tell the last two apart.
- [ ] A fix with native tests where the cause is in this firmware; otherwise the other repository's follow-up written in the report.
- [ ] §3a rewritten as listed; the 3.8 items ticked; `tools/verify.ps1` green.

## Out of scope

Exporting a node's private key so a radio can be given its old identity back. It may be worth having,
but it is a decision of Thomas's about where a private key may live: propose it in the report if the
cause makes it relevant.
