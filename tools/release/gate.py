#!/usr/bin/env python3
"""What a release is refused for, and which commit's ci run stands for the one being released.

    python tools/release/gate.py check --ref-name main --ref-input ""
    python tools/release/gate.py evidence --sha <40 hex>

`.github/workflows/release.yml` runs both in `ci-green`, **before anything is built**, so that a
release that cannot be published costs one job and not a matrix of them.

**The refusals** (docs/release.md, *When a release is refused*). Each is one sentence that says
what to do:

  * a release asked for from a ref that is not `main`, or one that names another `ref`. The
    button publishes `main`'s head; only a rehearsal may be pointed somewhere else;
  * no release note waiting in `release-notes/unreleased/`: there is nothing to release
    (tools/release/release_notes.py);
  * the tag of the version `VERSION` names already exists: the version was not moved on after the
    last release, which is what it looks like when the pull request that moves it did not land.
    The sentence says how to finish that by hand.

A rehearsal is refused for none of them.

**The evidence.** A release is refused on a commit ci has not passed, and the commit the release
workflow's own pull request leaves on `main` - `VERSION` and `release-notes/` and nothing else -
**may have no ci run of its own**: nothing starts a run for a push raised with the job's own
token, and the run GitHub does start for the pull request is held for a maintainer's approval and
is the branch's commit's in any case, not that of the commit the merge puts on `main`
(tools/release/notes_pr.py). Merged by a person it is an ordinary push and does have a run; the
walk is what covers the times it does not. `evidence_commit` walks back from
`main`'s head over exactly those commits - a commit that touched one other file stops the walk at
once - to the newest commit that does have a run, and that run is what `ci-green` waits for. The
images are still built from `main`'s head; what those commits carry cannot reach one.
"""

import argparse
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)

import release_notes  # noqa: E402
import version as version_module  # noqa: E402

TRUNK = "main"

# The files a release's own pull request may touch, and the only ones a commit with no ci run of
# its own may carry (release_notes.move writes exactly these two).
RELEASE_ONLY = ("VERSION", "release-notes/")

# How far back the walk goes before it gives up and waits for a run that will not come. Several
# releases in a row whose pull request landed and whose ci was then never run is already something
# a person should look at.
WALK_LIMIT = 20


class Refused(Exception):
    """Printed as it is, as the one sentence of a refusal."""


def release_only(paths):
    """True when every path given is one of the release's own files, and there is at least one."""
    paths = [path.strip() for path in paths if path.strip()]
    if not paths:
        return False
    return all(path == "VERSION" or path.startswith("release-notes/") for path in paths)


def evidence_commit(head, parent_of, changed_of, has_run, limit=WALK_LIMIT):
    """The commit whose ci run stands for `head`, and the commits walked over to reach it.

    `has_run(sha)` is whether ci.yml ever ran for that commit at all - green, red or still going.
    A commit that has one answers for itself. A commit that has none and carries only the
    release's own files is one this workflow left behind, so the walk goes to its parent; anything
    else is a commit ci is simply not finished with yet, and the caller waits for it.
    """
    commit = head
    walked = []
    for _ in range(limit):
        if has_run(commit) or not release_only(changed_of(commit)):
            return commit, walked
        parent = parent_of(commit)
        if not parent:
            return commit, walked
        walked.append(commit)
        commit = parent
    return commit, walked


def refusals(repo=REPO, ref_name=TRUNK, ref_input="", tags=()):
    """Everything a release is refused for, before anything is built. Returns the version."""
    if (ref_name or "").strip() != TRUNK:
        raise Refused(
            "a release is published from %s and this run was started on %r. Press *Run workflow* "
            "with the branch set to %s, or tick *dry run* to rehearse where you are "
            "(docs/release.md, *Making one*)." % (TRUNK, ref_name, TRUNK))
    if (ref_input or "").strip():
        raise Refused(
            "this run names the ref %r, and a release is always %s's head: only a *dry run* may "
            "rehearse somewhere else. Leave *ref* empty to publish, or tick *dry run* to rehearse "
            "on %s." % (ref_input, TRUNK, ref_input))

    version = version_module.read(repo)
    lines = release_notes.unreleased(repo)
    if not lines:
        raise Refused(
            "there is nothing to release: no release note is waiting in %s/. A task that changes "
            "something a person with a radio will notice leaves one line there, in its own pull "
            "request (tasks/RUNBOOK.md section 2)." % release_notes.UNRELEASED_DIR)

    tag = version_module.tag(version)
    if tag in set(tags):
        raise Refused(
            "%s is already a tag, so %s has been released and the version was not moved on after "
            "it. The release workflow does that in a pull request of its own; if that one is "
            "still open, merge it. If it was never opened, put %s in VERSION and move this "
            "release's lines out of %s/ into %s/%s.md on a branch, merge it, and press the button "
            "again (docs/release.md, *When a release is refused*)."
            % (tag, tag, version_module.next_patch(version), release_notes.UNRELEASED_DIR,
               release_notes.NOTES_DIR, tag))
    return version, lines


# ------------------------------------------------------------ what git and gh are asked, for real

def _git(repo, arguments):
    """git, with nothing written. The empty string when there is no git here: the container
    tools/verify.ps1 runs this step in has none, and a tag it cannot see is a tag that does not
    refuse a release - which is right, because this machine publishes nothing."""
    try:
        done = subprocess.run(["git", "-C", repo] + arguments, stdout=subprocess.PIPE,
                              stderr=subprocess.DEVNULL)
    except OSError:
        return ""
    if done.returncode != 0:
        return ""
    return done.stdout.decode("utf-8", "replace")


def tags_of(repo=REPO):
    """Every tag this checkout knows. release.yml fetches them with fetch-depth: 0."""
    return [line.strip() for line in _git(repo, ["tag", "--list"]).splitlines() if line.strip()]


def parent_of(repo=REPO):
    return lambda sha: _git(repo, ["rev-parse", "--verify", "--quiet", "%s^" % sha]).strip()


def changed_of(repo=REPO):
    def changed(sha):
        return [line.strip() for line
                in _git(repo, ["show", "--name-only", "--format=", sha]).splitlines()
                if line.strip()]
    return changed


def has_run_of(repository, workflow="ci.yml"):
    """Whether ci.yml has ever run for a commit - asked of GitHub, with the job's own token."""
    def has_run(sha):
        done = subprocess.run(["gh", "run", "list", "--repo", repository, "--workflow", workflow,
                               "--commit", sha, "--limit", "1", "--json", "status"],
                              stdout=subprocess.PIPE)
        if done.returncode != 0:
            raise Refused("gh could not say whether %s has a run of %s" % (sha[:7], workflow))
        return bool(json.loads(done.stdout.decode("utf-8") or "[]"))
    return has_run


# ---------------------------------------------------------------------------------- the command

def main(argv=None):
    parser = argparse.ArgumentParser(description="The refusals of a release, and the ci run that "
                                                 "stands for the commit being released.")
    parser.add_argument("what", choices=("check", "evidence"))
    parser.add_argument("--repo", default=REPO, help="the repository root")
    parser.add_argument("--ref-name", default=TRUNK, help="check: the ref the run was started on")
    parser.add_argument("--ref-input", default="", help="check: the run's `ref` input")
    parser.add_argument("--sha", default=None, help="evidence: the commit being released")
    parser.add_argument("--repository", default=os.environ.get("GITHUB_REPOSITORY", ""),
                        help="evidence: owner/name, for gh")
    args = parser.parse_args(argv)

    try:
        if args.what == "check":
            version, lines = refusals(args.repo, args.ref_name, args.ref_input, tags_of(args.repo))
            print("%s would be published as %s, with %d waiting line(s):"
                  % (TRUNK, version_module.tag(version), len(lines)))
            for name, text in lines:
                print("  %s: %s" % (name, text))
            return 0
        if not args.repository:
            raise Refused("evidence needs --repository owner/name (GITHUB_REPOSITORY)")
        sha, walked = evidence_commit(args.sha, parent_of(args.repo), changed_of(args.repo),
                                      has_run_of(args.repository))
        for skipped in walked:
            print("%s touches only the release's own files and has no ci run of its own; the run "
                  "that stands for it is further back" % skipped[:7], file=sys.stderr)
        print(sha)
    except (Refused, release_notes.NotesError, version_module.VersionError) as exc:
        sys.stderr.write("error: %s\n" % exc)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
