#!/usr/bin/env python3
"""The release step of tools/verify.ps1: the tooling's tests, then a dry run of the real thing.

    python tools/release/selfcheck.py --commit <40 hex> --build-dir .pio/build/<env> [--build-dir ...]

Two things, in this order, so that the release path is exercised on every commit without GitHub
and without the release key:

  1. tools/release/test_release.py - every test of the tooling - and tools/bench's own,
     test_bench.py and test_walk.py: the guards of the bench tool a session may touch a board
     with, and every walk of docs/bench.md run end to end, both against its fake serial layer
     (no board here and none needed).
  2. a dry run over the build this machine has just made: build_release.py writes out/release/,
     sign_manifest.py signs the manifest with the specification's public test key (which it would
     refuse to do for a release version), and spec/tools/manifestref.py verifies the signature.
     The same three steps the workflow runs, in the same order, on the same code.

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

import build_release  # noqa: E402
import manifestref  # noqa: E402
import sign_manifest  # noqa: E402
import test_release  # noqa: E402

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
    parser.add_argument("--only", choices=("tests", "dryrun"), default=None)
    args = parser.parse_args(argv)

    if args.only != "dryrun":
        print("== the release tooling's tests")
        ok, count = run_tests()
        if not ok:
            sys.stderr.write("error: the release tooling's tests failed\n")
            return 1
        print("%d test(s) passed" % count)

    if args.only != "tests":
        print("\n== a dry run of the release over this machine's build")
        try:
            dry_run(args.build_dir, args.commit, args.out, args.require_descriptor)
        except (build_release.ReleaseError, sign_manifest.SigningError) as exc:
            sys.stderr.write("error: %s\n" % exc)
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
