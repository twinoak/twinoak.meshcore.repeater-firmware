## The Walter it goes with

**`v0.1.4`** — the Walter firmware's current release, published 10 November 2026. It speaks
**link 1.2**, which is this firmware's link, so a unit takes this image with the Walter it already
has. A release of this repository that needs a newer Walter than that one says so in its own line
above, and this paragraph is edited in the same pull request.

This repository's release workflow cannot read the Walter's repository — one token, one
repository — so the Walter it goes with is written down here rather than looked up, and a release
never waits on the other box to be asked.

## Where the rest of it is

**How to flash it**, what to check afterwards and the admin password nobody knows until somebody
sets one: `docs/flashing.md`, linked under *Flashing one* below at this release's own tag. Which
file is the repeater and which is the scanner, which MeshCore this is, what every file in the
release is for and the application region an update may write are all under that divider too —
read out of the manifest this release carries, so they are this release's and not a memory of the
last one's.

**The key is unchanged** unless a line above says otherwise: release key `73efdd383a957596`, the
same one that signs the Walter firmware's releases (`keys/release.pub`).
