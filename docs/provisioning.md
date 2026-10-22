# Provisioning a unit

What a TwinOak repeater is the moment it leaves the flasher, and the one written
procedure for making it *this* unit — its name, its place, its passwords —
whether a person does it at the USB port or the Walter does it over the box link.

Everything below is upstream's own console interpreter. Nothing here is a
protocol of ours: the first-boot profile, a person at the field port and the
Walter over the link all speak the same command lines
(`src/roles/repeater/firstboot/Profile.h`, `docs/link.md` §2b).

Every reply quoted is MeshCore's at the pin `repeater-v1.18.0` (`81dbae59`),
read out of `src/helpers/CommonCLI.cpp`, `src/helpers/CommonRadioPrefs.cpp` (the
radio keys live there since that release) and
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

Six lines, because six is all this project has a document for — the
specification for one, the band's own regulation for the second, and the
**Danish standard** as Thomas settled it on 22 October 2026 for the last four:

```
# flooded adverts carry two-byte path hashes (spec/SYSTEM.md S-12, meshmaster.dk D-33)
set path.hash.mode 1
# the duty cycle the home channel's band allows, which upstream starts at 50 %
# (ERC Recommendation 70-03, Annex 1, 869.400-869.650 MHz; platformio.ini's [band_868])
set dutycycle 10
# the receiver's AGC and noise floor re-sampled every 4 s, which upstream leaves off
set agc.reset.interval 4
# a flood packet is repeated once at two-byte hashes, where upstream's own default repeats it twice
set loop.detect moderate
# an un-scoped flood is dropped past 15 hops, where upstream allows 64
set flood.max.unscoped 15
# the flood advert's period, 60-85 h from this node's own key (the rule is below). The 83 is
# the unit of the session in section 3, whose key begins 7f; another unit's is another number
set flood.advert.interval 83
```

| The line | Upstream's default | What it is for |
|---|---|---|
| `set path.hash.mode 1` | 0 — one byte | two-byte path hashes in the adverts this node floods (`spec/SYSTEM.md` S-12, meshmaster.dk D-33) |
| `set dutycycle 10` | 50 % | the band's legal ceiling, built from `platformio.ini`'s `[band_868].duty_cycle` |
| `set agc.reset.interval 4` | 0, which is off | the receiver's AGC and its noise-floor estimate re-sampled every four seconds, so that a repeater that has heard a strong neighbour recovers (`Dispatcher::loop` calls `resetAGC()` on that period; `RadioLibWrapper::resetAGC` throws the stuck floor away) |
| `set loop.detect moderate` | `minimal` since 1.18.0 (`off` before) | a flood packet is let past this node at most twice at one-byte path hashes and **once** at two or three — and this firmware floods at two (`max_loop_moderate`, `MyMesh::allowPacketForward`) |
| `set flood.max.unscoped 15` | 64 | an un-scoped flood (`ROUTE_TYPE_FLOOD`, no transport code) is not forwarded past 15 hops |
| `set flood.advert.interval <h>` | 47 hours, on every unit in the world | the period of this node's own flood advert, **60–85 hours derived from its key** (below), so that units flashed in one evening do not advertise in step |

**The advert interval is derived from the node's key**, and it is the one line
of the table that is *built* at run time rather than written:

> `h = 60 + (k mod 26)`, where **`k` is the first byte of the node's public
> key** — the first two hex digits of the `Repeater ID:` line, which is also
> the byte the mesh uses as this node's hash.

So 60 to 85 hours inclusive, every hour of that range reachable, and the same
key always giving the same number: a unit that is rebooted, or erased and given
back the same identity, asks for the same interval. `7f3a…` is `0x7f` = 127,
and 127 mod 26 = 23, so that unit floods an advert every **83 hours**. The
range sits well inside upstream's own, which takes 0 or 3–168 hours and refuses
everything else, and upstream re-arms the timer the moment the line is accepted,
so the derived period is in force on the first boot itself. The 26 hours of
spread are what keep a batch of units from flooding the mesh together for as
long as they run; upstream's single 47 is the thing being avoided.

Upstream floods this node's own adverts at `path_hash_mode + 1` bytes
(`MyMesh::sendSelfAdvertisement`), and its default of 0 means one byte — which
makes two repeaters that share a first hash byte both match a packet's next hop.
The mesh client's own floods already pass two in code; this is the half of S-12
that upstream decides from a preference.

The **duty cycle** is the band's and not a judgement: 869.618 MHz is in the
869.400–869.650 MHz sub-band of ERC Recommendation 70-03, Annex 1, where the
limit is 10 % of each hour at 500 mW e.r.p. Upstream starts a repeater at 50 %
(`airtime_factor` 1.0), which is five times that, so the number is in the band
beside the frequency and the power — `platformio.ini`'s `[band_868].duty_cycle`
— and **the line above is built from it** rather than written. The reply is
`OK - 10.0%`; `get dutycycle` reads it back. A **lower** value is still a
site's to choose, and a higher one is nobody's — but mind what upstream does
with it: 10 % is the lowest it can *keep*. `set dutycycle 5` is accepted and
takes effect, and then the next boot reads the saved value back through a clamp
of `airtime_factor` to 9.0 — exactly 100/10 − 1 — and the unit is at 10 % again.
So below the band's own value the setting is good for this boot and not for the
next one.

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
  trustworthy (`docs/ARCHITECTURE.md` §5), and a flood advert every 60 to 85
  hours — the number its own key gives it, where upstream would have every unit
  on 47.
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

**The three delays, and nothing else.** `rxdelay`, `txdelay` and
`direct.txdelay` follow how many neighbours the unit can hear, which a unit that
has never been switched on cannot know, so they are not in the profile and not
in the table above. Read them off meshguide.dk when the unit has been on the air
for a week; `tasks/phase-3/reports/03.md` has every `set` key of the pinned CLI
with the value upstream starts it at and the Danish standard's beside it.

The rest of that standard — `agc.reset.interval`, `loop.detect`,
`flood.max.unscoped` and the flood advert's interval — **is** in the profile
since 22 October 2026 (§1). Each of them is a ceiling or a mode and not a count
of neighbours, so a fresh unit can hold it without knowing anything about where
it stands; and each is still a site's to change afterwards, exactly like the
duty cycle below:

```
set flood.max.unscoped 20       -> OK                             a different hop ceiling
set loop.detect strict          -> OK                             a tighter mode
set agc.reset.interval 8        -> OK - interval rounded to 8      a longer AGC period
set flood.advert.interval 72    -> OK                             a period of your own choosing
```

Mind two things about those replies. `agc.reset.interval` is kept as **seconds
divided by four**, so upstream echoes back what it really stored —
`set agc.reset.interval 6` answers `OK - interval rounded to 4` — and
`get agc.reset.interval` reads the rounded number out again. And
`flood.advert.interval` takes 0 or 3–168 and refuses everything between 1 and 2
with `Error: interval range is 3-168 hours`; 0 turns the flood advert **off**,
which is a unit the mesh stops hearing about — and, since 1.18.0, a unit that
answers no DISCOVER request either (`MyMesh::isHiddenNode()`: both advert
intervals 0, and the local one *is* 0 on every unit of ours, because the first
`set` turned it off).

**The duty cycle is no longer one of them.** The 10 % of the 869.400–869.650 MHz
sub-band is the band's ceiling, not a judgement, so it is in the band and the
first boot asks for it (§1). What is left to a site is a *lower* value, which
is a judgement about the neighbourhood and is sent like any other setting:
`set dutycycle <1..100>`, `OK - <n>.<n>%` — and §1 says what upstream does with
one below 10 % on the next boot.

> **A unit provisioned before this firmware keeps upstream's defaults.** The
> profile runs only on the boot that finds no saved settings, and a unit that has
> been provisioned has them — `sensor get firstboot.state` says `never` on every
> boot since. Send the lines it missed once, at the USB port or over the link
> (§3, §4); each saves immediately and needs no reboot. The duty cycle is the one
> that matters most, because 50 % is five times what the band allows:
> `get dutycycle` → `> 10.0%` is how you know, and `> 50.0%` is a unit that has
> not been told. The four of the Danish standard are the same one-line fix —
> `set agc.reset.interval 4`, `set loop.detect moderate`,
> `set flood.max.unscoped 15` and `set flood.advert.interval <h>` with the hour
> §1's rule gives that unit's key, which `get public.key` prints.

## 3. At the USB port

The field port, 115200 baud, any terminal. The console echoes every character
and prefixes a reply with `  -> `. A line ends with carriage return.

```
Repeater ID: 7f3aa1b2...                      <- printed at boot
First boot: a random admin password was set (not shown; set yours with 'password <new>')
First boot: the profile ran 6 of 6 lines (sensor get firstboot.state says 'done'; docs/provisioning.md is the rest)

ver
  -> v1.18.0 (Build: 15 Oct 2026)
board
  -> RAK 4631
get radio
  -> > 869.618,62.5,8,8
sensor get firstboot.state
  -> > done
get path.hash.mode
  -> > 1
get dutycycle
  -> > 10.0%                                 <- the band's; 50.0% is a unit flashed before this
get agc.reset.interval
  -> > 4                                     <- the Danish standard's; upstream's own is 0
get loop.detect
  -> > moderate                              <- upstream's own is minimal (off before 1.18.0)
get flood.max.unscoped
  -> > 15                                    <- upstream's own is 64
get flood.advert.interval
  -> > 83                                    <- 60 + (0x7f mod 26), from the key above

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
     fw.version=twinoak-repeater 0.0.0-dev on MeshCore repeater-v1.18.0 (81dbae59)
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

An **older unit** — one provisioned before the duty cycle was the band's, or
before the Danish standard was in the profile — has five more lines to be given,
once, and they are the lines the profile itself runs:

```
get dutycycle
  -> > 50.0%                                 <- upstream's default, five times the band's
set dutycycle 10
  -> OK - 10.0%
get dutycycle
  -> > 10.0%

set agc.reset.interval 4
  -> OK - interval rounded to 4              <- upstream keeps seconds/4 and echoes it back
set loop.detect moderate
  -> OK
set flood.max.unscoped 15
  -> OK
get public.key
  -> > 7f3aa1b2...                            <- 0x7f = 127, 127 mod 26 = 23, so 60 + 23
set flood.advert.interval 83
  -> OK
```

Four things worth knowing about that session:

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
- **`get dutycycle` is the one line to read on a unit you did not flash.** The
  profile cannot have run on a unit that already had settings (§2), and 50 % is
  five times what the band allows. It is one line to fix and it saves at once.

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
| did the Danish standard reach me? | `get agc.reset.interval` → `> 4`, `get loop.detect` → `> moderate`, `get flood.max.unscoped` → `> 15` |
| how often do I flood an advert? | `get flood.advert.interval` → `> <60-85>`, and §1's rule says which from `get public.key` |
| who am I? | `get name`, `get lat`, `get lon`, `get owner.info`, `get public.key` |
| what scope do I flood in? | `region default`, `region` (the whole tree) |
| am I forwarding? | `get repeat` → `> on` |
| what firmware is this? | `sensor get fw.version`, and `ver` for upstream's own |
