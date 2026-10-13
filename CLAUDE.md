# Working in this repository

Read [`spec/SYSTEM.md`](spec/SYSTEM.md) first — what a unit is and the decisions
(`S-1`…) everything here rests on — then [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md).
For a task run by the driver, [`tasks/RUNBOOK.md`](tasks/RUNBOOK.md) is the
procedure and it wins over this file.

- **`spec/` is a copy.** Never edit a file under it. It is changed in
  `twinoak.meshcore.standard-repeater-design-v2/software/spec` and copied here
  with `specsync.py`; `python spec/tools/specsync.py verify spec` must pass.
- **MeshCore is not edited.** It is a pinned dependency under
  `.pio/libdeps/<env>/MeshCore`. Read it there; cite file, function and pin
  when code depends on a line of it.
- **The trunk is `main`.** One task, one branch `task/pP-NN-<name>`, one PR.
- **Verify locally with `tools/verify.ps1`** before pushing anything. CI runs
  the same script and is not where debugging happens.
- **Sibling repositories are read-only**, and `meshmaster.dk` and
  `twinoak.meshmaster.exitnode-firmware` (named `meshmaster-agent-firmware`
  until 11 October 2026) may have other sessions working in them: read
  with `git -C ../<folder> show <commit>:<path>` or `gh api`, never check out
  or write there.
- **The hardware's interfaces are frozen** (`INTERCONNECT.md` in the hardware
  repository). Firmware adapts to the pins; the pins do not change.
- Never enable `MESH_PACKET_LOGGING` or `MESH_DEBUG`, never store or print a
  password, never write outside the application region, never chip-erase.
- New logic goes in host-portable files with native tests; the code that
  touches the board stays thin.
