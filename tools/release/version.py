#!/usr/bin/env python3
"""The version this repository is about to release. One file, and nowhere else.

    python tools/release/version.py                -> 0.1.4   (what the next release is called)
    python tools/release/version.py --next         -> 0.1.5   (what it becomes once that is out)
    python tools/release/version.py --set 0.2.0             (a bigger step, edited beforehand)

`VERSION` at the root of the repository holds `X.Y.Z` and one newline. It names **the release that
comes next**, not the one that went out last: `.github/workflows/release.yml` reads it when a
person presses *Run workflow*, builds that version into every image
(`-D TWINOAK_FW_VERSION`, so the descriptor and the manifest carry it with the commit), tags the
release `vX.Y.Z` - and then moves this file on to the next patch in a pull request of its own
(tools/release/release_notes.py, `move`). A bigger step than a patch is one line edited here
before the button is pressed, in the pull request that earns it.

So the number is in the repository rather than in a tag somebody types, and the release workflow
reads it from here and from no other file. A release whose tag already exists is refused
(tools/release/gate.py): that is what "the version was not moved on" looks like from the outside.
"""

import argparse
import os
import re
import sys

REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
VERSION_FILE = "VERSION"

# X.Y.Z and nothing else. A pre-release or build metadata is something a *build* appends
# (0.0.0-dryrun+<sha7>); it is never what this file holds.
VERSION = re.compile(r"^(\d+)\.(\d+)\.(\d+)$")


class VersionError(Exception):
    """Printed as it is."""


def version_path(repo=REPO):
    return os.path.join(repo, VERSION_FILE)


def read(repo=REPO):
    """The X.Y.Z of VERSION: the release that comes next."""
    path = version_path(repo)
    if not os.path.isfile(path):
        raise VersionError(
            "%s is not in the repository, and it is the one place the version of the next release "
            "is written (tools/release/version.py). One line, X.Y.Z." % VERSION_FILE)
    with open(path, "r", encoding="utf-8") as fh:
        text = fh.read()
    lines = [line.strip() for line in text.splitlines() if line.strip()]
    if len(lines) != 1 or not VERSION.match(lines[0]):
        raise VersionError(
            "%s holds %r; it holds one line of X.Y.Z and nothing else - the version the next "
            "release is published as." % (VERSION_FILE, text))
    return lines[0]


def tag(version):
    """`v0.1.4` of `0.1.4`. The tag is the version with a v in front and nothing else."""
    if not VERSION.match((version or "").strip()):
        raise VersionError("%r is not a version this repository releases (X.Y.Z)" % version)
    return "v" + version.strip()


def next_patch(version):
    """0.1.4 -> 0.1.5. What the workflow writes back after a release."""
    match = VERSION.match((version or "").strip())
    if not match:
        raise VersionError("%r is not a version this repository releases (X.Y.Z)" % version)
    major, minor, patch = (int(part) for part in match.groups())
    return "%d.%d.%d" % (major, minor, patch + 1)


def write(version, repo=REPO):
    """Put X.Y.Z in VERSION, with one newline and LF endings whatever the platform is."""
    if not VERSION.match((version or "").strip()):
        raise VersionError("%r is not a version this repository releases (X.Y.Z)" % version)
    with open(version_path(repo), "wb") as fh:
        fh.write((version.strip() + "\n").encode("utf-8"))
    return version.strip()


def main(argv=None):
    parser = argparse.ArgumentParser(description="The version of the release that comes next.")
    parser.add_argument("--repo", default=REPO, help="the repository root")
    parser.add_argument("--next", action="store_true", help="print the patch after it instead")
    parser.add_argument("--tag", action="store_true", help="print vX.Y.Z rather than X.Y.Z")
    parser.add_argument("--set", metavar="X.Y.Z", default=None,
                        help="write this version and print it")
    args = parser.parse_args(argv)
    try:
        if args.set:
            version = write(args.set, args.repo)
        else:
            version = read(args.repo)
            if args.next:
                version = next_patch(version)
        print(tag(version) if args.tag else version)
    except VersionError as exc:
        sys.stderr.write("error: %s\n" % exc)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
