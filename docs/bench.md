# The bench — what no session could verify

What every task left under *Not verified here*, collected in the order the rig
is brought up (`hardware/pcb/BENCH-BREADBOARD.md` §7 in the hardware
repository): UART, reset, SWD, flash, then the kill line and I2C. Each line
says which task wrote it, so the report with the detail is one hop away
(`tasks/phase-P/reports/NN.md`). Tick what the bench confirmed; a line that
fails goes back to the task's follow-ups.

## 0. Before the rig

- [ ] **p0-01** Nothing flashed: the image of phase 0 is upstream's stock
  `simple_repeater` for the RAK4631 built with our channel, and the first
  thing worth flashing is phase 1's. If it is flashed anyway (UF2 or
  `adafruit-nrfutil dfu serial -pkg firmware.zip`), it should boot, print
  `Repeater ID:` on the USB console at 115200 and advertise on 869.618 MHz,
  62.5 kHz, SF8, CR8 — and it carries upstream's default admin password, which
  phase 3's provisioning replaces.
- [ ] **p0-02** **The release key, which only Thomas can make.**
  `python spec/tools/manifestref.py keygen`, then the private hex into the
  repository secret `RELEASE_SIGNING_KEY` and the public hex into
  `keys/release.pub` in a commit (`keys/README.md`). Until it exists a tag is
  refused, which is the safe state, and the tag path of
  `.github/workflows/release.yml` has never run — only its dry run, which is
  the same jobs with `gh release create` left out.
- [ ] **p0-02** **The raw `.bin` written over SWD boots.** The release's
  `twinoak-repeater-rak4630-single-868-vX.Y.Z.bin` is the application bytes
  alone, to be written at the `load_address` the manifest gives (`0x00026000`
  today) with nothing else erased. The Walter's flasher does this in service;
  on the bench it is a J-Link or a CMSIS-DAP with an explicit base address, for
  example `pyocd flash -t nrf52840 --base-address 0x26000 <file>` or
  `openocd … -c "program <file> 0x26000 verify reset exit"`. What is being
  checked: the board comes up exactly as it does from the `.uf2` of the same
  release — same console line, same advert — and the file system, the identity
  and the bootloader are untouched afterwards. No session can do it: nothing
  was flashed and no radio was heard.

## 1. The repeater on the rig

(phase 1, task 01)

## 2. The link from a laptop

(phase 1, task 03)

## 3. The clock

(phase 1, task 04)

## 4. Management over the link

(phase 1, task 05)

## 5. Power

(phase 1, task 06)
