#!/usr/bin/env python3
"""The hand-written top of a release's body, from release-notes/vX.Y.Z.md in this repository.

    python tools/release/release_notes.py --version 0.1.0+abc1234 --require    > out/NOTES.md
    python tools/release/release_notes.py --version 0.0.0-dryrun+abc1234       > out/NOTES.md
    python tools/release/release_notes.py --version 0.1.0+abc1234 --check      (the path, no body)

A release body is two parts, in this order (docs/release.md, *The notes*):

  1. **what a person reads**, written by hand before the tag and kept in the repository as
     `release-notes/v<X.Y.Z>.md`: what this release is, which MeshCore it is, where the guide is,
     and which Walter release it goes with. It is reviewed in the pull request that precedes the
     tag, like everything else here;
  2. **what the tooling generates** from the manifest - the files, what each one is for, the
     application region, the version the firmware reports - which `.github/workflows/release.yml`
     appends after this.

**A tag with no such file is refused**, the way a tag with no signing key is, and in the same job:
`release.yml`'s `ci-green`, before anything is built. The reason is that nothing else would catch
it - the body is written once, by a person, and a release whose notes are only the generated table
is a release nobody can read. A rehearsal is not refused: its version is `0.0.0-dryrun+<sha7>`, no
file is written for it, and this prints one line saying so in place of the top.

The version is given as the release's own (`X.Y.Z+<sha7>`, or `0.0.0-dryrun+<sha7>`); the file is
named by the `X.Y.Z` of it and nothing else, because that is what the tag is.
"""

import argparse
import os
import re
import sys

REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
NOTES_DIR = "release-notes"

# X.Y.Z, and then whatever a build or a rehearsal appended: -dryrun, -dev, +<sha7>.
VERSION = re.compile(r"^(\d+\.\d+\.\d+)([-+].*)?$")

# What separates the two parts of a body. The blank line above it matters: a `---` directly under
# a paragraph is a setext heading in Markdown, and would turn the last line of the hand-written
# part into one.
DIVIDER = "\n---\n\n"


class NotesError(Exception):
    """Printed as it is."""


def tag_of(version):
    """`v0.1.0` of `0.1.0+abc1234`, and of `0.1.0`. The tag is the X.Y.Z and nothing else."""
    match = VERSION.match((version or "").strip())
    if not match:
        raise NotesError("%r is not a version this release tooling writes (X.Y.Z, with an "
                         "optional -pre-release or +build metadata after it)" % version)
    return "v" + match.group(1)


def notes_path(version, repo=REPO):
    """Where the hand-written top of this version's notes lives, relative to the repository."""
    return os.path.join(repo, NOTES_DIR, tag_of(version) + ".md")


def hand_written(version, repo=REPO):
    """The text of release-notes/vX.Y.Z.md, ready to stand at the top of a release body.

    Raises NotesError when there is no such file or it holds nothing: an empty file is a file
    somebody meant to write and did not, and publishing it would say nothing.
    """
    path = notes_path(version, repo)
    relative = os.path.relpath(path, repo).replace(os.sep, "/")
    if not os.path.isfile(path):
        raise NotesError(
            "%s is not in the repository, so the release notes of %s have no hand-written top and "
            "this tag is not released. Write that file on a branch, in the pull request that "
            "precedes the tag: what this release is, which MeshCore it is, the link to "
            "docs/flashing.md, and which Walter release it goes with (docs/release.md, *The "
            "notes*). Then tag the commit it merged as." % (relative, tag_of(version)))
    with open(path, "r", encoding="utf-8") as fh:
        text = fh.read()
    if not text.strip():
        raise NotesError("%s holds nothing. A release body's hand-written top is what a person "
                         "reads first; an empty one is a file somebody meant to write and did "
                         "not." % relative)
    return text.strip() + "\n" + DIVIDER


def no_hand_written(version, repo=REPO):
    """What a rehearsal prints in place of the top: there is none, and why that is not a failure."""
    relative = os.path.relpath(notes_path(version, repo), repo).replace(os.sep, "/")
    return ("**No hand-written top.** `%s` is not in the repository, which is what a rehearsal of "
            "version `%s` expects: a *tag* without that file is refused in `ci-green`, before "
            "anything is built (`tools/release/release_notes.py`).\n" % (relative, version)
            + DIVIDER)


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="The hand-written top of a release's body, from %s/vX.Y.Z.md." % NOTES_DIR)
    parser.add_argument("--version", required=True,
                        help="the release's version: X.Y.Z+<sha7>, or 0.0.0-dryrun+<sha7>")
    parser.add_argument("--require", action="store_true",
                        help="a release: there must be such a file, or this fails")
    parser.add_argument("--check", action="store_true",
                        help="print the path and not the body (for a job that only refuses)")
    parser.add_argument("--repo", default=REPO, help="the repository root")
    args = parser.parse_args(argv)

    try:
        if args.require or os.path.isfile(notes_path(args.version, args.repo)):
            text = hand_written(args.version, args.repo)
        else:
            text = no_hand_written(args.version, args.repo)
    except NotesError as exc:
        sys.stderr.write("error: %s\n" % exc)
        return 1
    if args.check:
        print(os.path.relpath(notes_path(args.version, args.repo), args.repo).replace(os.sep, "/"))
        return 0
    # The bytes, as UTF-8, and not through whatever encoding the console happens to have: these
    # notes become a release body, and on Windows sys.stdout would quietly write cp1252 - an em
    # dash as 0x97 - into a file GitHub then reads as UTF-8 (found while diffing the rehearsed
    # body against the published one, p3.7-01). A stdout a test has replaced has no .buffer.
    stream = getattr(sys.stdout, "buffer", None)
    if stream is None:
        sys.stdout.write(text)
    else:
        sys.stdout.flush()
        stream.write(text.encode("utf-8"))
        stream.flush()
    return 0


if __name__ == "__main__":
    sys.exit(main())
