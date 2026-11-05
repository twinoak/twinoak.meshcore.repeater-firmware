# `keys/` — the public half, and only ever the public half

A release of this firmware is signed where it is built and checked where it is
used (`spec/SYSTEM.md` S-8, `spec/FIRMWARE-MANIFEST.md` §3). One Ed25519 key
does that, and it has two halves that live in two very different places.

| | Where it lives | Who may see it |
|---|---|---|
| private | the repository secret `RELEASE_SIGNING_KEY`, and nowhere else | nobody; GitHub holds it and the release workflow passes it to `tools/release/sign_manifest.py` in an environment variable |
| public | `keys/release.pub`, in this repository | everybody — it proves nothing by itself |

**The private half never reaches a machine of ours.** Every job of
`.github/workflows/release.yml` runs on a GitHub-hosted runner, which begins
each job new and is gone after it; the organisation's own Unraid runners, where
everything that merely *checks* now runs, never see the secret and never build
what is signed (README.md, *Where CI runs*; `docs/release.md`).
`tools/ci/check-runners.ps1` is the guard, and it runs as `ci.yml`'s first
step.

**Nothing secret goes in this directory.** Not the private key, not a copy of
it "for convenience", not a key file with the private half commented out. A
private key that has been in a working tree is a key to replace.

## `release.pub`

**One line, in the form both repositories write a key in** — the trust table's
line, which is what the Walter firmware's `keys/release-keys.txt` carries for
this same key:

```
ed25519 <key id> <64 hex>
```

`<64 hex>` is the raw 32-byte Ed25519 public key and `<key id>` is the first
eight bytes of SHA-256 over it, which is also what a signature names itself by.
The 64 hexadecimal characters **alone** are read too — that is what
`manifestref.py keygen` prints — and lines beginning with `#`, and blank lines,
are ignored, so the file can say who made the key and when.

The key id is **recomputed from the key** and has to match the one the line
names: both halves of that line are written by a person, and a line whose id was
edited to say something the key does not say is refused
(`tools/release/sign_manifest.py`, `public_key_of_line`). A release is never
signed against a file this reader would not take, and `tools/verify.ps1` reads
**this** file with **that** reader on every commit, so a malformed one fails on a
commit rather than at a tag, after the builds.

### The key that is here

```
ed25519 73efdd383a957596 …
```

made by Thomas on **20 October 2026** (`82e2534`), private half in this
repository's `RELEASE_SIGNING_KEY` secret and in nothing else, and the same key
the Walter firmware trusts. `v0.1.0` is the first release it signed.

## Making it — once, by a person

```
python spec/tools/manifestref.py keygen
```

prints `private`, `public` and `key id`. Then:

1. Put the **private** hex into the repository secret `RELEASE_SIGNING_KEY`
   (*Settings → Secrets and variables → Actions → New repository secret*).
   Paste it there and nowhere else; do not keep the terminal scrollback.
2. Put the **public** half into `keys/release.pub` in a commit, as the one line
   above: `ed25519 <key id> <64 hex>`, which both repositories read.
3. The public half is also what the Walter firmware is built to trust
   (`twinoak.meshcore.walter-firmware`; a Walter build trusts a small fixed
   list, room for three, so a key can be replaced by shipping a Walter that
   trusts both and then one that trusts only the new one).

A Walter with no release key compiled in accepts no update at all, which is the
state before a key exists and is safe.

## What the key signs

The bytes of `manifest.json`, exactly as they are in the release, and nothing
else — `manifest.json.sig` is one line, `ed25519 <key id> <base64>`. Because the
manifest carries the SHA-256 of every file of the release, that one signature
covers the whole release.

## The test key is not this key

`spec/fixtures/manifest/TEST-KEY.txt` holds a key pair that is **public on
purpose**: every repository that tests signature checking needs the same one. It
signs rehearsals — the dry run of the release workflow, and the local one
`tools/verify.ps1` makes on every commit — and `sign_manifest.py` refuses to
sign a release version with it. A firmware build that trusts it is a test build.
