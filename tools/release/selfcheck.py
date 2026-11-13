#!/usr/bin/env python3
"""The release step of tools/verify.ps1: the tooling's tests, then a dry run of the real thing.

    python tools/release/selfcheck.py --commit <40 hex> --build-dir .pio/build/<env> [--build-dir ...]

Four things, in this order, so that the release path is exercised on every commit without GitHub
and without the release key:

  0. what only a release reads and no dry run touches: the **real** keys/release.pub, through
     sign_manifest.read_public_key - the reader the release path uses - then VERSION, the one
     place the next release's version is written, then every line waiting in
     release-notes/unreleased/ held to its format, the standing paragraph under them, the body's
     top they compile into, and every release-notes/vX.Y.Z.md already published. A dry run signs
     with the specification's test key and generates its own body, so none of it is in its way:
     before p3.7-01 keys/release.pub was in the form the Walter firmware's trust table uses and
     the reader refused it, and a tag would have failed at `publish` after the builds. All of it
     now fails here instead, on a commit (tasks/phase-3.7/01-the-first-release.md,
     tasks/phase-3.11/01-a-release-from-the-button.md).
  1. tools/release/test_release.py - every test of the tooling - and tools/bench's own,
     test_bench.py and test_walk.py: the guards of the bench tool a session may touch a board
     with, and every walk of docs/bench.md run end to end, both against its fake serial layer
     (no board here and none needed).
  2. the base under the application, tools/base/check.py: the vendored file of boards/base/ is
     the one its pin names (SHA-256), passes spec/FIRMWARE-MANIFEST.md section 4a against the
     application region of every build directory's linker script, and its README says the same
     numbers. A changed byte fails here, on this machine, before any release.
  3. a dry run over the build this machine has just made: build_release.py writes out/release/
     (the base under every RAK4630 image), sign_manifest.py signs the manifest with the
     specification's public test key (which it would refuse to do for a release version, and
     refuses for a manifest without its base), and spec/tools/manifestref.py verifies the
     signature. The same three steps the workflow runs, in the same order, on the same code.

It is one command because it needs the `cryptography` of requirements.txt, and on a machine
without it that means one container run rather than three (tools/verify.ps1 decides which).
Nothing here publishes anything and nothing here sees the release key.
"""

import argparse
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(REPO, "spec", "tools"))
sys.path.insert(0, os.path.join(REPO, "tools", "base"))

import build_release  # noqa: E402
import check as base_check  # noqa: E402  (tools/base/check.py: the base under the application)
import manifestref  # noqa: E402
import release_notes  # noqa: E402
import sign_manifest  # noqa: E402
import test_release  # noqa: E402
import version as version_module  # noqa: E402

# tools/bench's guards (phase 3.3's task 02): Python already, standard library only, and the
# place a refusal must be proved on every commit is a step that runs without a board - this one.
sys.path.insert(0, os.path.join(REPO, "tools", "bench"))
import test_bench  # noqa: E402
import test_walk  # noqa: E402  (phase 3.3's task 03: the walks of docs/bench.md)


def run_tests():
    suite = unittest.TestSuite()
    for module in (test_release, test_bench, test_walk):
        suite.addTests(unittest.defaultTestLoader.loadTestsFromModule(module))
    result = unittest.TextTestRunner(verbosity=2, stream=sys.stdout).run(suite)
    return result.wasSuccessful(), result.testsRun


def the_files_only_a_release_reads(repo=REPO):
    """keys/release.pub with the release path's own reader, and everything the notes are made of.

    None of it is on the dry run's path below - that signs with the test key and builds its own
    body - so without this a malformed one is found when the button is pressed, after the builds,
    and by then the only way out is another commit (docs/release.md, *When a release is refused*).
    """
    path = os.path.join(repo, sign_manifest.PUBLIC_KEY_FILE)
    public = sign_manifest.read_public_key(path)
    print("%s: a %d-byte Ed25519 public key, key id %s (the form keys/README.md describes)"
          % (sign_manifest.PUBLIC_KEY_FILE, len(public), manifestref.key_id(public)))

    # The one place the next release's version is written, and the tag it would take.
    number = version_module.read(repo)
    print("%s: %s, so the next release is %s"
          % (version_module.VERSION_FILE, number, version_module.tag(number)))

    # The lines the tasks have left, each held to the format the release compiles them in.
    waiting = release_notes.unreleased(repo)
    if not waiting:
        print("%s/: no line is waiting, so a release would be refused (there is nothing to say)"
              % release_notes.UNRELEASED_DIR)
    for name, text in waiting:
        print("%s/%s: %d characters, %s" % (release_notes.UNRELEASED_DIR, name, len(text), text))

    # And the body's top those lines make, which is what the button would publish today.
    release_notes.standing(repo)
    top = release_notes.top(repo, require=False)
    print("the body's top for %s: %d characters, %d line(s) in front of the generated part"
          % (version_module.tag(number), len(top), top.count("\n")))

    for name in release_notes.published(repo):
        version = name[1:-3] if name.startswith("v") else name[:-3]
        text = release_notes.hand_written(version, repo)
        print("%s/%s: %d characters, %d line(s), as published"
              % (release_notes.NOTES_DIR, name, len(text), text.count("\n")))


def dry_run(build_dirs, commit, out_dir, require_descriptor=False):
    """build, sign with the test key, verify - and say what came out."""
    build_release.clean(out_dir)
    manifest = build_release.build_release(build_dirs, commit, out_dir,
                                           require_descriptor=require_descriptor, log=print)
    manifest_path = os.path.join(out_dir, "manifest.json")
    public = sign_manifest.sign_manifest(manifest_path, use_test_key=True, log=print)
    with open(manifest_path, "rb") as fh:
        message = fh.read()
    with open(manifest_path + ".sig", "r", encoding="ascii") as fh:
        if not manifestref.verify([public], message, fh.read()):
            raise build_release.ReleaseError(
                "the signature just written does not verify against the test public key")
    print("manifest.json.sig: valid against the test key (manifestref.verify)")
    for name in sorted(os.listdir(out_dir)):
        print("  %s  %d bytes" % (name, os.path.getsize(os.path.join(out_dir, name))))
    return manifest


def main(argv=None):
    parser = argparse.ArgumentParser(description="The release tooling's tests and a local dry run.")
    parser.add_argument("--build-dir", action="append", default=[], metavar="DIR",
                        help="a built environment's directory; once per image")
    parser.add_argument("--commit", required=True, help="the 40 hexadecimal characters of the commit")
    parser.add_argument("--out", default=os.path.join("out", "release"))
    parser.add_argument("--require-descriptor", action="store_true")
    parser.add_argument("--only", choices=("keys", "tests", "base", "dryrun"), default=None)
    args = parser.parse_args(argv)

    if args.only in (None, "keys"):
        print("== what only a release reads: keys/release.pub, VERSION, and the notes")
        try:
            the_files_only_a_release_reads()
        except (sign_manifest.SigningError, release_notes.NotesError,
                version_module.VersionError) as exc:
            sys.stderr.write("error: %s\n" % exc)
            return 1

    if args.only in (None, "tests"):
        print()
        print("== the release tooling's tests")
        ok, count = run_tests()
        if not ok:
            sys.stderr.write("error: the release tooling's tests failed\n")
            return 1
        print("%d test(s) passed" % count)

    if args.only in (None, "base"):
        print("\n== the base under the application, against every build's linker script")
        base_args = []
        for build_dir in args.build_dir:
            base_args += ["--build-dir", build_dir]
        if not base_args:
            sys.stderr.write("error: no build directory to read the application region from\n")
            return 1
        if base_check.main(base_args) != 0:
            sys.stderr.write("error: the base is not the pinned one (tools/base/check.py)\n")
            return 1

    if args.only in (None, "dryrun"):
        print("\n== a dry run of the release over this machine's build")
        try:
            dry_run(args.build_dir, args.commit, args.out, args.require_descriptor)
        except (build_release.ReleaseError, sign_manifest.SigningError) as exc:
            sys.stderr.write("error: %s\n" % exc)
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
