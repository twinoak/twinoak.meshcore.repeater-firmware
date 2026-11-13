#!/usr/bin/env python3
"""The top of a release's body: the lines the tasks left, and what no manifest can generate.

    python tools/release/release_notes.py top     > out/NOTES.md   (the body's top, as published)
    python tools/release/release_notes.py lines                    (the waiting lines, in order)
    python tools/release/release_notes.py move                     (after a release: publish, move on)

**A task that changes something a person with a radio will notice leaves one line**, in its own
pull request, as `release-notes/unreleased/<task>.md`: one line, at most 240 characters, plain
words, about what will be noticed rather than about what was done. A change nobody can see - a
test, a refactoring, a workflow, a document - leaves no file (tasks/RUNBOOK.md section 2). Nothing
is summarised afterwards, because the moment the change is made is the only moment anybody knows
what it looks like from outside.

A release body is then two parts, in this order (docs/release.md, *The notes*):

  1. **this**: the waiting lines, in the order the tasks merged, and under them
     `release-notes/standing.md` - what every hand-written top used to repeat and no manifest can
     generate, *The Walter it goes with* first among it, which the task that changes it edits;
  2. **what the tooling generates** from the manifest - which MeshCore this is, the files, what
     each one is for, the application region, where the flashing guide is - which
     `.github/workflows/release.yml` appends after this.

**A release with no waiting line is refused** before anything is built (tools/release/gate.py):
a release with nothing to say is a release nobody reads. A *rehearsal* is never refused; with
nothing waiting it prints one line saying so in place of the lines.

After the release is published the workflow calls `move`: the lines become
`release-notes/vX.Y.Z.md` - the body's top, as published, which is what the four tops written by
hand before this are - `release-notes/unreleased/` is emptied, and `VERSION` moves to the next
patch (tools/release/version.py). That move is one pull request, opened and merged by the
workflow itself.
"""

import argparse
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)

import version as version_module  # noqa: E402

NOTES_DIR = "release-notes"
UNRELEASED_DIR = "release-notes/unreleased"
STANDING_NAME = "standing.md"
STANDING_FILE = "release-notes/standing.md"

# One line, and a bound on it: a release body that opens with a wall of text is one nobody reads,
# and the generated part below it already says everything a manifest knows. Two sentences fit.
MAX_LINE = 240

# X.Y.Z, and then whatever a build or a rehearsal appended: -dryrun, -dev, +<sha7>.
VERSION = re.compile(r"^(\d+\.\d+\.\d+)([-+].*)?$")

# What separates the two parts of a body. The blank line above it matters: a `---` directly under
# a paragraph is a setext heading in Markdown, and would turn the last line of the top into one.
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
    """Where this version's notes live once they are published, relative to the repository."""
    return os.path.join(repo, NOTES_DIR, tag_of(version) + ".md")


def published(repo=REPO):
    """The names of the published notes: release-notes/vX.Y.Z.md, and not standing.md."""
    directory = os.path.join(repo, NOTES_DIR)
    if not os.path.isdir(directory):
        return []
    return sorted(name for name in os.listdir(directory)
                  if name.endswith(".md") and name != STANDING_NAME)


def hand_written(version, repo=REPO):
    """The text of release-notes/vX.Y.Z.md: a release's body as it was published.

    Written by hand for v0.1.0 to v0.1.3 and by `move` from this release on; either way it is the
    top of that release's body and nothing else. Raises NotesError when there is no such file or
    it holds nothing.
    """
    path = notes_path(version, repo)
    relative = os.path.relpath(path, repo).replace(os.sep, "/")
    if not os.path.isfile(path):
        raise NotesError(
            "%s is not in the repository, so %s has no published notes. A release writes that "
            "file itself, out of the lines waiting in %s/ (docs/release.md, *The notes*)."
            % (relative, tag_of(version), UNRELEASED_DIR))
    with open(path, "r", encoding="utf-8") as fh:
        text = fh.read()
    if not text.strip():
        raise NotesError("%s holds nothing. A release body's top is what a person reads first; "
                         "an empty one is a file somebody meant to write and did not." % relative)
    return text.strip() + "\n" + DIVIDER


# -------------------------------------------------------------- the line a task leaves behind

def unreleased_dir(repo=REPO):
    return os.path.join(repo, *UNRELEASED_DIR.split("/"))


def line_of(name, repo=REPO):
    """The one line of release-notes/unreleased/<name>, held to the format, or NotesError."""
    path = os.path.join(unreleased_dir(repo), name)
    relative = "%s/%s" % (UNRELEASED_DIR, name)
    with open(path, "r", encoding="utf-8") as fh:
        lines = [line.strip() for line in fh.read().splitlines() if line.strip()]
    if len(lines) != 1:
        raise NotesError(
            "%s is %d line(s); a release note is one line - what a person with a radio will "
            "notice, in a sentence or two (tasks/RUNBOOK.md section 2)." % (relative, len(lines)))
    text = lines[0]
    if len(text) > MAX_LINE:
        raise NotesError("%s is %d characters; the bound is %d (tasks/RUNBOOK.md section 2)."
                         % (relative, len(text), MAX_LINE))
    if text[0] in "-*#>|":
        raise NotesError("%s begins with %r; a release note is plain words - no list marker, no "
                         "heading, no table (tasks/RUNBOOK.md section 2)." % (relative, text[0]))
    return text


def _git(repo, arguments):
    """git, with nothing written: the empty string when there is no git or no repository here."""
    try:
        done = subprocess.run(["git", "-C", repo] + arguments, stdout=subprocess.PIPE,
                              stderr=subprocess.DEVNULL)
    except OSError:
        return ""
    if done.returncode != 0:
        return ""
    return done.stdout.decode("utf-8", "replace")


def merge_order(repo=REPO):
    """The names of release-notes/unreleased/*.md in the order the tasks merged.

    The commit that ADDED each file, along the trunk's first parent, so a file that arrived in a
    merged pull request is ordered by that merge and not by its file name (meshmaster.dk's
    tools/ci/Compile-ReleaseNotes.ps1 looks the same commit up for the same reason). A file git
    does not know yet - the one the task running right now is adding, or a scratch repository with
    no commits - comes after the ones it does, by name, which is the only order there is for it.
    """
    directory = unreleased_dir(repo)
    present = sorted(name for name in os.listdir(directory)
                     if name.endswith(".md")) if os.path.isdir(directory) else []
    known = []
    for arguments in (["log", "--reverse", "--first-parent", "--diff-filter=A", "--format=%H",
                       "--name-only", "--", UNRELEASED_DIR],
                      ["log", "--reverse", "--diff-filter=A", "--format=%H", "--name-only",
                       "--", UNRELEASED_DIR]):
        for raw in _git(repo, arguments).splitlines():
            line = raw.strip()
            if not line.startswith(UNRELEASED_DIR + "/"):
                continue
            name = line[len(UNRELEASED_DIR) + 1:]
            if name in present and name not in known:
                known.append(name)
        if len(known) == len(present):
            break
    return known + [name for name in present if name not in known]


def unreleased(repo=REPO):
    """[(name, line)] of the waiting notes, in the order the tasks merged."""
    return [(name, line_of(name, repo)) for name in merge_order(repo)]


def standing(repo=REPO):
    """release-notes/standing.md: what every top repeats and no manifest can generate."""
    path = os.path.join(repo, *STANDING_FILE.split("/"))
    if not os.path.isfile(path):
        raise NotesError(
            "%s is not in the repository, and it is what every release body says that no manifest "
            "can generate - *The Walter it goes with* first among it. The task that changes one "
            "of those things edits that file (docs/release.md, *The notes*)." % STANDING_FILE)
    with open(path, "r", encoding="utf-8") as fh:
        text = fh.read()
    if not text.strip():
        raise NotesError("%s holds nothing." % STANDING_FILE)
    return text.strip() + "\n"


def compiled_top(repo=REPO, version=None):
    """The top of the body of the release VERSION names: the lines, then the standing paragraph.

    The same bytes for a release and for a rehearsal of one - that is what makes a rehearsal worth
    running - so the heading carries the version the repository names and not the build's own
    0.0.0-dryrun.
    """
    version = version or version_module.read(repo)
    lines = unreleased(repo)
    if not lines:
        raise NotesError(
            "no release note is waiting in %s/, so there is nothing to release. A task that "
            "changes something a person with a radio will notice leaves one line there, in its "
            "own pull request (tasks/RUNBOOK.md section 2); a release with nothing to say is one "
            "nobody reads, and that is refused before anything is built." % UNRELEASED_DIR)
    body = ["## What is new in %s" % version_module.tag(version), ""]
    body += ["- %s" % text for _, text in lines]
    return "\n".join(body) + "\n\n" + standing(repo)


def nothing_waiting(repo=REPO, version=None):
    """What a rehearsal prints in place of the lines: there are none, and why that is no failure."""
    version = version or version_module.read(repo)
    return ("**No release note is waiting.** `%s/` is empty, so `%s` has nothing to say yet and a "
            "release of it is refused before anything is built; a rehearsal is not "
            "(`tools/release/gate.py`).\n"
            % (UNRELEASED_DIR, version_module.tag(version)))


def top(repo=REPO, version=None, require=True):
    """The body's top with the divider under it. require=False: a rehearsal, which may have none."""
    try:
        text = compiled_top(repo, version)
    except NotesError:
        if require:
            raise
        text = nothing_waiting(repo, version)
    return text.strip() + "\n" + DIVIDER


# -------------------------------------------------------------- the move, after a release is out

def move(repo=REPO, version=None):
    """Publish the waiting lines as release-notes/vX.Y.Z.md, empty unreleased/, move VERSION on.

    What the workflow runs once the release is out, in the working tree it then files as one pull
    request. Returns (the version released, the one after it, the names it took).
    """
    version = version or version_module.read(repo)
    text = compiled_top(repo, version)
    names = [name for name, _ in unreleased(repo)]
    path = notes_path(version, repo)
    if os.path.exists(path):
        raise NotesError("%s is already in the repository: %s has been published before, and its "
                         "notes are not written twice."
                         % (os.path.relpath(path, repo).replace(os.sep, "/"),
                            version_module.tag(version)))
    with open(path, "wb") as fh:
        fh.write(text.encode("utf-8"))
    for name in names:
        os.remove(os.path.join(unreleased_dir(repo), name))
    after = version_module.next_patch(version)
    version_module.write(after, repo)
    return version, after, names


# ---------------------------------------------------------------------------------- the command

def _write_utf8(text):
    """The bytes, as UTF-8, and not through whatever encoding the console happens to have: these
    notes become a release body, and on Windows sys.stdout would quietly write cp1252 - an em dash
    as 0x97 - into a file GitHub then reads as UTF-8 (found while diffing the rehearsed body
    against the published one, p3.7-01). A stdout a test has replaced has no .buffer."""
    stream = getattr(sys.stdout, "buffer", None)
    if stream is None:
        sys.stdout.write(text)
    else:
        sys.stdout.flush()
        stream.write(text.encode("utf-8"))
        stream.flush()


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="The top of a release's body, from %s/ and %s."
                    % (UNRELEASED_DIR, STANDING_FILE))
    parser.add_argument("what", choices=("top", "lines", "move"),
                        help="top: the body's top. lines: the waiting lines. move: publish them")
    parser.add_argument("--repo", default=REPO, help="the repository root")
    parser.add_argument("--version", default=None,
                        help="the version being released; VERSION's own unless given")
    parser.add_argument("--rehearsal", action="store_true",
                        help="top: nothing waiting is a line saying so rather than a refusal")
    args = parser.parse_args(argv)

    try:
        if args.what == "top":
            _write_utf8(top(args.repo, args.version, require=not args.rehearsal))
        elif args.what == "lines":
            for name, text in unreleased(args.repo):
                print("%s: %s" % (name, text))
        else:
            released, after, names = move(args.repo, args.version)
            print("%s/%s.md: %d line(s) out of %s/, which is now empty"
                  % (NOTES_DIR, version_module.tag(released), len(names), UNRELEASED_DIR))
            print("%s: %s -> %s" % (version_module.VERSION_FILE, released, after))
    except (NotesError, version_module.VersionError) as exc:
        sys.stderr.write("error: %s\n" % exc)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
