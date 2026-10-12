# Provisioning a unit

What a TwinOak repeater is the moment it leaves the flasher, and the one written
procedure for making it *this* unit — its name, its place, its passwords —
whether a person does it at the USB port or the Walter does it over the box link.

Everything below is upstream's own console interpreter. Nothing here is a
protocol of ours: the first-boot profile, a person at the field port and the
Walter over the link all speak the same command lines
(`src/roles/repeater/firstboot/Profile.h`, `docs/link.md` §2b).

Every reply quoted is MeshCore's at the pin `repeater-v1.17.1` (`d9296435`),
read out of `src/helpers/CommonCLI.cpp` and
`examples/simple_repeater/MyMesh.cpp` — not off a board, because no session that
wrote this page had one. `docs/bench.md` §10 is the walk that confirms it.

## 1. Out of the box

A board flashed with a TwinOak image and switched on, with nobody near it, is
already a member of the Danish mesh. Three things decide what it is, in this
order:

| | Decided by | What it is |
|---|---|---|
| the **channel** | the build (`platformio.ini`'s `[band_868]`) | 869.618 MHz, 62.5 kHz, SF8, CR8, +22 dBm — MeshCore Denmark, EU Narrow. It is in the image and **not** in the profile below, so a unit is on the right frequency even if every other step failed |
| the **admin password** | the first boot, from the radio's own noise | fifteen random alphanumerics that **nobody knows**, drawn on the boot that finds no saved settings (`spec/SYSTEM.md` S-13, `src/roles/repeater/app/AdminPassword.h`). It is never printed and never on the air |
| the **profile** | the first boot, as console lines | the table below |
| everything else | upstream's `MyMesh` constructor | §4 lists every `set` key with the value it starts at |

### The profile, as shipped

One line, because one line is all a document of this project gives a value for:

```
# flooded adverts carry two-byte path hashes (spec/SYSTEM.md S-12, meshmaster.dk D-33)
set path.hash.mode 1
```

Upstream floods this node's own adverts at `path_hash_mode + 1` bytes
(`MyMesh::sendSelfAdvertisement`), and its default of 0 means one byte — which
makes two repeaters that share a first hash byte both match a packet's next hop.
The mesh client's own floods already pass two in code; this is the half of S-12
that upstream decides from a preference.

The profile runs **once**, on the boot that finds neither `/prefs.json` nor the
legacy `/com_prefs`, after the random admin password and before the radio is
used — nothing has been sent or received at that point. It prints one line on the
USB console and leaves its outcome where three different people can read it:

```
sensor get firstboot.state
  -> > done
```

`done`, `partial: <n> refused`, or `never` because this boot found settings and
ran nothing. A line that is refused is counted, the rest still run, and the
console names the first refusal. The state is **per boot**: a unit provisioned
last month says `never`, which is the right answer to "did this boot touch my
settings".

### What it means in practice

- **It repeats.** `disable_fwd` is 0, so forwarding is on from the first second.
- **Nobody can administer it over the mesh.** The admin password is random and
  unknown, so no phone can log in as admin until a person or the Walter sets one
  (§2). That is the point of S-13, and it is also the reason the USB port and the
  box link are the only two ways in to a fresh unit.
- **Anybody can log in as a guest.** `guest_password` starts **empty**, and
  upstream answers a blank login by comparing it with the guest password —
  which an empty one matches (`MyMesh::handleLoginReq`). A stranger's blank
  login therefore succeeds at guest permission. §2 is where that is decided
  on purpose rather than by default.
- **It advertises itself.** A zero-hop advert goes out once the clock is
  trustworthy (`docs/ARCHITECTURE.md` §5), and a flood advert every 47 hours.
  The *local* advert timer starts at two minutes and is turned off by the first
  preference save of any manual configuration — upstream does that itself
  (`CommonCLI::savePrefs` zeroes `advert_interval` below 60 minutes, "now that
  device has been manually configured"), and the profile's one line is a manual
  configuration as far as upstream is concerned. So a provisioned unit sends no
  two-minute adverts; the 47-hour flood advert and the boot advert are what
  announce it.
- **Its name is `repeater` and its position is 0, 0.** Which is exactly why §2
  exists.
- **It has an identity.** A random Ed25519 key pair, made on the first boot and
  kept in the file system. `erase` on the console wipes the file system, so it
  wipes the **identity** as well as the settings: a unit that has been erased is
  a new node to the mesh and to MeshMaster.

## 2. Making it a unit

Six settings, and what each one is for. Nothing else has to be touched to put a
unit on the air.

| What | The line | Reply | Notes |
|---|---|---|---|
| its **name**, as every phone sees it | `set name TwinOak-Odense` | `OK` | 31 characters. `[ ] \ : , ? *` are refused (`Error, bad chars`); spaces are not |
| its **latitude** | `set lat 55.3250` | `OK` | decimal degrees. It goes in the advert, because upstream's advert location policy starts at `prefs` |
| its **longitude** | `set lon 10.4900` | `OK` | |
| who to call | `set owner.info Thomas Eg\|thomas@twinoak.dk` | `OK` | 119 characters; a `\|` is stored as a newline and read back as a `\|` |
| the **admin password** | `password <new>` | `password now: <new>` | 15 characters; longer is truncated to 15. This is the only copy until MeshMaster is told it (§3) |
| the **guest password** | `set guest.password hello` | `OK` | 15 characters. Leaving it empty is a decision, not a default: an empty one makes **every blank login succeed at guest** (§1) |

And, if the network uses them, the **regions** (scopes). The Danish mesh's
default scope is `#dk`:

```
region default dk
  ->  default scope is now dk
region save
  -> OK
```

Those answer with a sentence rather than with `OK`, and every failure of theirs
begins with `Err` — which is the rule `linkcli.py provision` judges them by.
**`region load` is the one line never to send over the link**: it puts the
interpreter into a mode that only a blank line ends, and a blank line is not a
legal `CLI` request (`docs/link.md` §2b). Define regions with `region def` or
`region put`.

### What this page deliberately does not set

The Danish mesh has a best-practice profile beyond the two bytes of S-12 —
meshguide.dk's `lib/checks.js` is it, and it is the document to fill the table
from. Most of its recommendations are **per site**: `rxdelay`, `txdelay` and
`direct.txdelay` follow how many neighbours the unit can hear, and the duty
cycle, `loop.detect` and `flood.max.unscoped` are a judgement about the
neighbourhood. A unit that has never been switched on cannot know any of them,
so they are not in the profile and not in the table above. Read them off
meshguide.dk when the unit has been on the air for a week, and
`tasks/phase-3/reports/03.md` has every key with the value upstream starts it at.

## 3. At the USB port

The field port, 115200 baud, any terminal. The console echoes every character
and prefixes a reply with `  -> `. A line ends with carriage return.

```
Repeater ID: 7f3aa1b2...                      <- printed at boot
First boot: a random admin password was set (not shown; set yours with 'password <new>')
First boot: the profile ran 1 of 1 lines (sensor get firstboot.state says 'done'; docs/provisioning.md is the rest)

ver
  -> v1.17.1 (Build: 14 Aug 2026)
board
  -> RAK 4631
get radio
  -> > 869.618,62.5,8,8
sensor get firstboot.state
  -> > done
get path.hash.mode
  -> > 1

set name TwinOak-Odense
  -> OK
set lat 55.3250
  -> OK
set lon 10.4900
  -> OK
set owner.info Thomas Eg|thomas@twinoak.dk
  -> OK
set guest.password hello
  -> OK
region default dk
  ->  default scope is now dk
region save
  -> OK
password <the one you are going to keep>
  -> password now: <the one you are going to keep>

get name
  -> > TwinOak-Odense
get public.key
  -> > 7f3aa1b2...                            <- 64 hex digits; MeshMaster needs this
sensor list
  -> 10 vars
     fw.version=twinoak-repeater 0.0.0-dev on MeshCore repeater-v1.17.1 (d9296435)
     walter.wake=0
     link.errors=0
     walter.seen=never
     clock.state=unset
     ... next:                                <- the digit is cut; see below
sensor list 5
  -> 10 vars
     power.low_mv=3500
     power.crit_mv=3300
     obs.wake=0
     client.refused=0
     firstboot.state=done
```

Three things worth knowing about that session:

- **`password <new>` echoes the password back.** Upstream does that on purpose
  ("just to let admin know for sure"), so the password is on the screen of
  whatever is plugged into the field port. That is a reason to provision at a
  bench and not over somebody else's terminal — and it is why the link tool in
  §4 prints stars instead.
- **`sensor list` is two pages, and the first one ends a digit short.** Upstream
  stops a listing once it has written 134 bytes of a 160-byte reply, so the ten
  keys do not fit one page — and with ten of them upstream's own header is
  `10 vars` rather than `9 vars`, which makes the first page exactly 160
  characters where it used to be 159. A reply is 159 characters and a NUL, here
  and on the link alike, so the last character of `... next:5` is lost: the
  digit, and nothing else. The hint that there is more is still there, `sensor
  list 5` is the rest, and `sensor get <key>` always works whatever the paging
  does (`src/roles/repeater/app/Settings.cpp` has the arithmetic and
  `test/test_settings` measures it).
- **A power cut in the middle is safe.** Every `set` saves the preferences, so
  what has been typed is kept and the rest can be typed again. The *first boot*
  is the one that is all-or-nothing, and it is arranged that way deliberately:
  the admin password and the profile are written in one save, so a unit is
  either unprovisioned and retried on the next boot or finished
  (`src/roles/repeater/app/FirstBoot.h`).

## 4. Over the link

The same lines, as `CLI` requests — which is what the Walter will do. From a
laptop with a USB-UART dongle on the link pins (`docs/link.md` §1 has the
wiring), the lines go in a text file and `provision` sends them one at a time:

```
python3 tools/linkcli.py --port COM7 provision unit-odense.txt
python3 tools/linkcli.py --port COM7 provision unit-odense.txt --dry-run
```

**The file format is the first-boot profile's own.** A `#` line is a comment, a
blank line is nothing, and every other line is a console command sent exactly as
it stands — leading and trailing spaces included, because a trailing space is
part of the value upstream stores (`set guest.password ` with the space is how an
empty one is restored).

```
# TwinOak-Odense, provisioned <date> - docs/provisioning.md section 2
set name TwinOak-Odense
set lat 55.3250
set lon 10.4900
set owner.info Thomas Eg|thomas@twinoak.dk
set guest.password hello
# the default scope of the Danish mesh
region default dk
region save
# the admin password. Printed as stars, here and in the reply
password <the one you are going to keep>
```

What the tool does with it:

```
provision unit-odense.txt: 8 line(s)
  2:   set name TwinOak-Odense
       -> OK
  ...
  8:   region default dk
       ->  default scope is now dk
  11:  password ****
       -> accepted (the reply to a password line is not printed)
every line was taken
```

- **It stops at the first refusal**, says which line of the file it was, and
  sends nothing after it. Exit code 1. A provisioning file is a sequence a
  person wrote: a refusal in the middle of it means the rest was written against
  a different firmware or a different board, and sending it anyway would leave a
  half-configured unit nobody told.
- **A password is never printed.** Not the value in the line — `password ****`,
  four stars whatever its length — and not the reply either, because upstream's
  `password` branch echoes the new password and its refusals can echo the whole
  line. For a password line the tool prints `accepted` or `refused` and no more.
- **It judges a reply the way the firmware does.** Upstream has no status code:
  the outcome of a line is the English in its reply, and almost every acceptance
  it can produce begins with `OK` or `ok`. The two families that do not get
  their own rule, read off upstream's own branches: `password <new>` is taken
  when the reply begins `password now:`, and a `region` line is taken when the
  reply does *not* begin `Err`.
- **A file is read before anything is sent.** A line over 160 bytes, a control
  character in one, a `region load`, or one of the four commands that restart the
  node and never answer (`reboot`, `poweroff`, `shutdown`, `clkreboot`) is
  refused with the file and the line number, and the link is never opened. Use
  `--dry-run` to see exactly what would go.

`tools/linkloop/check.sh` drives all of this against the firmware's own session
behind a pipe, with no board at all: the accepted file, the refusal, the
`region load`, the empty file, and a check that the password's value appears
nowhere in the output.

## 5. Afterwards: telling MeshMaster

A provisioned unit is not yet an adopted one. Two things have to reach
MeshMaster, and one of them exists nowhere else:

1. **The node's public key**, which is its name on the mesh.
   `python3 tools/linkcli.py --port COM7 identity` prints it (`pubkey`, with the
   advert `name` beside it), and `get public.key` on the console prints the same
   64 hex digits. It is stable as long as the file system is: an `erase` makes a
   new one.
2. **The admin password that was just set.** MeshMaster's agent logs in to a
   repeater with it, and **the copy set in §2 or §3 is the only copy there is**.
   Nothing in this firmware can read it back out — `get` has no branch for it,
   and the one reply that echoes it is the `password` command's own, at the
   moment it is set. A password that is set and not written down is a unit that
   has to be erased and provisioned again.

`spec/proposals/01-the-unit-in-meshmaster.md` is what MeshMaster does with the
two; the rest of adoption is its side and not this firmware's.

## 6. Reading it back

One line each, from the console, over the link (`linkcli.py cli "<line>"`), or
from a phone logged in as admin:

| Question | The line |
|---|---|
| did this boot provision me? | `sensor get firstboot.state` |
| am I on the right channel? | `get radio` → `> 869.618,62.5,8,8` |
| do my adverts carry two-byte hashes? | `get path.hash.mode` → `> 1` |
| who am I? | `get name`, `get lat`, `get lon`, `get owner.info`, `get public.key` |
| what scope do I flood in? | `region default`, `region` (the whole tree) |
| am I forwarding? | `get repeat` → `> on` |
| what firmware is this? | `sensor get fw.version`, and `ver` for upstream's own |
