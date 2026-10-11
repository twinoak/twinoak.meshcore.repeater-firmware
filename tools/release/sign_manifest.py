#!/usr/bin/env python3
"""Writes manifest.json.sig: one Ed25519 signature over the bytes of a manifest.

    RELEASE_SIGNING_KEY=<64 hex> python tools/release/sign_manifest.py out/release/manifest.json
    python tools/release/sign_manifest.py --test-key out/release/manifest.json     (a rehearsal)

**The key is read from the environment and from nowhere else.** Never an argument, never a file in
this repository, never printed - not even in an error message, which is why every message below
says what is wrong without quoting what it was given. The signature is made where the image is
built and checked where it is used (spec/SYSTEM.md S-8, spec/FIRMWARE-MANIFEST.md section 3);
spec/tools/manifestref.py does the arithmetic, and this script only decides what may be signed:

  * **A release is signed or it is not a release.** With no RELEASE_SIGNING_KEY there is no
    signature and no release, and the message says how the key is made.
  * **The specification's test key signs rehearsals only.** It is public on purpose
    (spec/fixtures/manifest/TEST-KEY.txt), so a manifest whose version is a plain X.Y.Z release
    version is refused with it: an artifact signed by the test key can never be mistaken for a
    release.
  * **The public half must be the one this repository publishes.** On the real path the signature
    this key makes must name keys/release.pub's key id, so a release cannot go out signed by a key
    no Walter will trust.

The verification that follows - sign, then prove the signature passes before anything is published
- is spec/tools/manifestref.py's own command, over the public key alone:

    python spec/tools/manifestref.py verify <public hex> out/release/manifest.json
"""

import argparse
import json
import os
import re
import sys

REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
sys.path.insert(0, os.path.join(REPO, "spec", "tools"))

import manifestref  # noqa: E402  (after sys.path; spec/ is a copy and is never edited)

KEY_VARIABLE = "RELEASE_SIGNING_KEY"
PUBLIC_KEY_FILE = os.path.join("keys", "release.pub")
TEST_KEY_FIXTURE = os.path.join("spec", "fixtures", "manifest", "index.json")

# A release version is X.Y.Z with optional +build metadata and no pre-release part. Anything with a
# pre-release part - 0.0.0-dev, 0.0.0-dryrun+abc1234 - is a rehearsal, and only a rehearsal may be
# signed with the specification's test key.
RELEASE_VERSION = re.compile(r"^\d+\.\d+\.\d+(\+[0-9A-Za-z.-]+)?$")

HOW_TO_MAKE_THE_KEY = (
    "A release is signed or it is not a release. The key is made once, by a person:\n"
    "    python spec/tools/manifestref.py keygen\n"
    "The private half becomes the repository secret %s and exists nowhere else - not in\n"
    "a file, not in a commit, not in a log. The public half goes in %s and is compiled\n"
    "into the Walter firmware (spec/FIRMWARE-MANIFEST.md section 3; keys/README.md)."
    % (KEY_VARIABLE, PUBLIC_KEY_FILE)
)


class SigningError(Exception):
    """Printed as it is. It never carries a key."""


def is_release_version(version):
    """X.Y.Z, with or without +build metadata: what a tag releases. 0.0.0-dryrun+sha7 is not."""
    return bool(RELEASE_VERSION.match(version or ""))


def read_public_key(path):
    """The 32-byte public key of a .pub file: one line of 64 hexadecimal characters, with # comment
    lines and blank lines allowed above it."""
    if not os.path.isfile(path):
        raise SigningError("%s is not there.\n%s" % (path, HOW_TO_MAKE_THE_KEY))
    with open(path, "r", encoding="ascii", errors="replace") as fh:
        for line in fh:
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            if not re.match(r"^[0-9a-fA-F]{64}$", line):
                raise SigningError("%s does not hold 64 hexadecimal characters on its first line "
                                   "of content" % path)
            return bytes.fromhex(line)
    raise SigningError("%s holds no key.\n%s" % (path, HOW_TO_MAKE_THE_KEY))


def test_public_key(repo=REPO):
    """The specification's own test key, from the fixture that generated it - not derived here, so
    that this script needs no key arithmetic of its own."""
    with open(os.path.join(repo, TEST_KEY_FIXTURE), "r", encoding="utf-8") as fh:
        return bytes.fromhex(json.load(fh)["test_public_key"])


def _private_from_environment(environ):
    raw = (environ.get(KEY_VARIABLE) or "").strip()
    if not raw:
        raise SigningError("%s is not set, so this manifest cannot be signed.\n%s"
                           % (KEY_VARIABLE, HOW_TO_MAKE_THE_KEY))
    if not re.match(r"^[0-9a-fA-F]{64}$", raw):
        raise SigningError("%s is not 64 hexadecimal characters (it is %d characters long). Its "
                           "value is not shown here, on purpose." % (KEY_VARIABLE, len(raw)))
    return bytes.fromhex(raw)


def sign_manifest(manifest_path, use_test_key=False, environ=None, public_key_file=None, log=None):
    """Writes <manifest_path>.sig and returns the public key a verifier is to use."""
    log = log or (lambda line: None)
    environ = os.environ if environ is None else environ
    public_key_file = public_key_file or os.path.join(REPO, PUBLIC_KEY_FILE)

    try:
        with open(manifest_path, "rb") as fh:
            message = fh.read()
    except IOError as exc:
        raise SigningError("the manifest is not readable: %s" % exc)
    try:
        version = json.loads(message.decode("utf-8"))["version"]
    except (ValueError, KeyError, UnicodeDecodeError):
        raise SigningError("%s is not a manifest with a version in it" % manifest_path)

    if use_test_key:
        if is_release_version(version):
            raise SigningError(
                "this manifest says version %s, which is a release, and the specification's test "
                "key is public (spec/fixtures/manifest/TEST-KEY.txt): a release signed with it "
                "would be accepted by every test build in existence. Sign a release with %s, or "
                "give the rehearsal a version that is not a release version "
                "(0.0.0-dryrun+<sha7>)." % (version, KEY_VARIABLE))
        private, public = manifestref.TEST_PRIVATE, test_public_key()
    else:
        private, public = _private_from_environment(environ), read_public_key(public_key_file)

    # manifestref.sign names the key it used by its key id. Holding that against the public half
    # this repository publishes is the whole check, and it needs no key arithmetic here.
    line = manifestref.sign(private, message)
    signed_by = line.strip().split(" ")[1]
    if signed_by != manifestref.key_id(public):
        raise SigningError(
            "%s is not the key %s publishes (the signature names key id %s, the published key is "
            "%s), so a Walter that trusts this repository's key would refuse the release. Neither "
            "key is shown here." % (KEY_VARIABLE, public_key_file, signed_by,
                                    manifestref.key_id(public)))
    with open(manifest_path + ".sig", "w", encoding="ascii", newline="\n") as fh:
        fh.write(line)
    log("signed %s for version %s with key id %s%s"
        % (os.path.basename(manifest_path), version, signed_by,
           " (the specification's test key: a rehearsal)" if use_test_key else ""))
    return public


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Writes manifest.json.sig. The private key comes from $%s." % KEY_VARIABLE)
    parser.add_argument("manifest", help="the manifest.json to sign")
    parser.add_argument("--test-key", action="store_true",
                        help="sign with the specification's public test key (rehearsals only)")
    parser.add_argument("--public-key-file", default=None,
                        help="the public half to hold the key against (default: %s)" % PUBLIC_KEY_FILE)
    parser.add_argument("--print-public", action="store_true",
                        help="also print the public key, so the next step can verify with it")
    parser.add_argument("--public-out", default=None, metavar="FILE",
                        help="write the public key's hex there, for the verification step")
    args = parser.parse_args(argv)

    try:
        public = sign_manifest(args.manifest, use_test_key=args.test_key,
                               public_key_file=args.public_key_file, log=print)
    except SigningError as exc:
        sys.stderr.write("error: %s\n" % exc)
        return 1
    if args.public_out:
        with open(args.public_out, "w", encoding="ascii", newline="\n") as fh:
            fh.write(public.hex() + "\n")
    if args.print_public:
        print(public.hex())
    return 0


if __name__ == "__main__":
    sys.exit(main())
