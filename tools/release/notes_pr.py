#!/usr/bin/env python3
"""The pull request a release leaves behind: what it says, and how the step that files it ends.

    python tools/release/notes_pr.py body --tag v0.1.4 --next 0.1.5 \
        --branch release-notes/v0.1.4 --release-url https://github.com/<o>/<r>/releases/tag/v0.1.4
    python tools/release/notes_pr.py outcome --pushed yes --opened yes --merged no \
        --tag v0.1.4 --branch release-notes/v0.1.4 --url https://github.com/<o>/<r>/pull/50

`.github/workflows/release.yml`'s last step, once the release is out, moves the published lines
into `release-notes/<tag>.md` and `VERSION` on and offers that one commit to `main` as a pull
request of its own (`release_notes.py move`). `main` requires a pull request before merging and
GitHub's bypass lists take people, teams, roles and GitHub Apps and never the Actions token, so
the job opens one rather than pushing; and `main` requires the `verify` check, so the job cannot
merge the pull request it opened.

**A release that published is not a failure.** Where that pull request is open and only a person
can merge it, nothing failed: the release is out and is not undone, and the backstop is the next
release's refusal - the tag `VERSION` names already exists (gate.py) - so this cannot be
forgotten. The step therefore ends **green**, with one line at the top of the run's summary
saying what is left and which pull request. Red is kept for what did fail: the branch could not
be pushed, or no pull request could be opened for it. Nothing is waiting for a person then,
because there is nothing to merge. twinoak.meshcore.walter-firmware's release.yml (`waiting()`)
is the model for ending green; it ends green for those two as well, and here they are red.

**What is true about that pull request's check**, as seen on 13 November 2026 when v0.1.4 was the
first release from the button (tasks/phase-3.12/02-the-release-says-what-is-left.md):

  * `main` requires `verify`, so the merge is refused with *Required status check "verify" is
    expected* - that is every release, and tasks/phase-3.11/reports/01.md says so;
  * GitHub **does** start a `ci` run for the pull request the job opened, and holds it for a
    maintainer's approval. v0.1.4's was run 43467131534: no job of it was ever created, and about
    an hour later it was listed as a failed run - the second red run of that evening. The body
    the job wrote said *No check runs on this pull request*, which is not what happens;
  * so there are two ways to finish it and either is right: approve the run and merge when
    `verify` is green, or merge without waiting, because the commit touches `VERSION` and
    `release-notes/` and nothing else. The body says both.

One place decides how the step ends, and it is tested (tools/release/test_release.py): pushed or
not, opened or not, merged or not, in; the exit code and the summary's first line, out.
"""

import argparse
import collections
import os
import sys

# (the exit code of the step, the one line that is both the top of the run's summary and the
# annotation on the job, the summary as Markdown)
Outcome = collections.namedtuple("Outcome", ("code", "headline", "summary"))

# What a person does with the pull request, said once and used in both places - the run's summary
# and the pull request's own body - because whichever of the two is read first has to be enough.
TWO_WAYS = (
    "- **Approve the run and merge it when `verify` is green.** *Actions* -> the `ci` run for "
    "this branch -> *Approve and run*.",
    "- **Or merge it without waiting.** The commit is `VERSION` and `release-notes/` and nothing "
    "else; `ci` runs on `main` when it lands, and the release after this one stands on that run - "
    "or, where there is none, on the run of the commit below it (`tools/release/gate.py`).",
)

BACKSTOP = ("The release stands either way, and the next release is refused while `VERSION` "
            "still names a version that has been published (`docs/release.md`, *When a release "
            "is refused*).")


def body(tag, next_version, branch, release_url=""):
    """The pull request's body: what the commit is, why a person merges it, and the two ways.

    It says what is true about the check - that `main` requires `verify` so the job cannot merge
    this itself, and that the `ci` run GitHub starts for it is held for a maintainer's approval -
    where before it said that no check runs on it at all.
    """
    out = [
        "The notes of **%s**, moved out of `release-notes/unreleased/` into "
        "`release-notes/%s.md` as published, and `VERSION` moved on to %s."
        % (tag, tag, next_version),
    ]
    if release_url:
        out += ["", "The release is already out and is not undone: %s" % release_url]
    out += [
        "",
        "Opened by the publish job of `.github/workflows/release.yml` from the branch `%s` "
        "(`tools/release/release_notes.py move`): `main` requires a pull request before merging, "
        "and GitHub cannot give the Actions token a bypass." % branch,
        "",
        "**It is merged by a person.** `main` requires the `verify` check, so the job that opened "
        "this cannot merge it; and the `ci` run GitHub starts for this pull request is held for a "
        "maintainer's approval, so unapproved it never runs and after about an hour is listed as "
        "failed. Either way of finishing is right:",
        "",
    ]
    out += list(TWO_WAYS)
    out += ["", BACKSTOP]
    return "\n".join(out) + "\n"


def outcome(pushed, opened, merged, tag, branch, url=""):
    """How the step ends, and the first line of the run's summary. The only place that decides.

    `pushed`, `opened` and `merged` are what the three commands of the step did. Everything else
    is what the run knows: the tag, the branch the commit was pushed to, and the pull request's
    URL once there is one.
    """
    where = url or branch
    if not pushed:
        headline = ("%s is published and is not undone; only the branch `%s`, which files its "
                    "notes and moves `VERSION` on, could not be pushed." % (tag, branch))
        return Outcome(1, headline, _summary(headline, [
            "Nothing is waiting for a person to merge, because there is no pull request: the "
            "commit never reached `origin`.",
            "",
            "Do it by hand: `python tools/release/release_notes.py move` on a branch off `main`, "
            "then a pull request. " + BACKSTOP,
        ]))
    if not opened:
        headline = ("%s is published and is not undone; the branch `%s` is on `origin`, but no "
                    "pull request could be opened for it." % (tag, branch))
        return Outcome(1, headline, _summary(headline, [
            "*Settings -> Actions -> General -> Allow GitHub Actions to create and approve pull "
            "requests* is the setting that lets the job open one.",
            "",
            "Open a pull request from `%s` by hand and merge it. " % branch + BACKSTOP,
        ]))
    if not merged:
        headline = ("%s is published. One thing is left for a person: merge %s, which files its "
                    "notes and moves `VERSION` on." % (tag, where))
        return Outcome(0, headline, _summary(headline, [
            "**Nothing failed.** `main` requires the `verify` check, so this job cannot merge the "
            "pull request it opened, and the `ci` run GitHub starts for that pull request is held "
            "for a maintainer's approval. Either way of finishing is right:",
            "",
        ] + list(TWO_WAYS) + [
            "",
            BACKSTOP,
        ]))
    headline = "%s is published and its notes are on `main` as %s." % (tag, where)
    return Outcome(0, headline, _summary(headline, []))


def _summary(headline, rest):
    """The summary: that one line first, as its own paragraph, and whatever follows it."""
    lines = ["**%s**" % headline]
    if rest:
        lines += [""] + list(rest)
    return "\n".join(lines).rstrip("\n") + "\n"


def annotation(result):
    """The one line again, as the job's own annotation, so the run's list says it too."""
    return "::%s::%s" % ("notice" if result.code == 0 else "error",
                         result.headline.replace("`", "").replace("*", ""))


# ---------------------------------------------------------------------------------- the command

TRUE = ("yes", "y", "true", "1")
FALSE = ("no", "n", "false", "0", "")


def flag(text, what):
    """yes or no, as a step of a shell writes it. Anything else is a mistake and says so."""
    value = (text or "").strip().lower()
    if value in TRUE:
        return True
    if value in FALSE:
        return False
    raise ValueError("--%s takes yes or no, not %r" % (what, text))


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="The body of the pull request a release leaves behind, and how the step that "
                    "files it ends.")
    parser.add_argument("what", choices=("body", "outcome"))
    parser.add_argument("--tag", required=True, help="vX.Y.Z, the release that is out")
    parser.add_argument("--branch", required=True, help="the branch the commit was pushed to")
    parser.add_argument("--next", dest="next_version", default="",
                        help="body: the version VERSION was moved on to")
    parser.add_argument("--release-url", default="", help="body: the release that is already out")
    parser.add_argument("--url", default="", help="outcome: the pull request, once there is one")
    parser.add_argument("--pushed", default="no", help="outcome: did the branch reach origin")
    parser.add_argument("--opened", default="no", help="outcome: was a pull request opened")
    parser.add_argument("--merged", default="no", help="outcome: did it merge")
    parser.add_argument("--summary", default=None,
                        help="outcome: where the summary is appended; $GITHUB_STEP_SUMMARY unless "
                             "given, and nowhere when there is neither")
    args = parser.parse_args(argv)

    if args.what == "body":
        if not args.next_version:
            sys.stderr.write("error: body needs --next, the version VERSION was moved on to\n")
            return 2
        sys.stdout.write(body(args.tag, args.next_version, args.branch, args.release_url))
        return 0

    try:
        result = outcome(flag(args.pushed, "pushed"), flag(args.opened, "opened"),
                         flag(args.merged, "merged"), args.tag, args.branch, args.url)
    except ValueError as exc:
        sys.stderr.write("error: %s\n" % exc)
        return 2

    path = args.summary if args.summary is not None else os.environ.get("GITHUB_STEP_SUMMARY", "")
    if path:
        # Appended, and after a blank line: the step above it has already written the move to the
        # same file, and a paragraph of Markdown runs into the line before it otherwise.
        with open(path, "a", encoding="utf-8") as fh:
            fh.write("\n" + result.summary)
    # The annotation goes to stdout, where the runner reads a workflow command from; the summary
    # goes to the file, because a step's stdout is not where GitHub collects one.
    print(annotation(result))
    return result.code


if __name__ == "__main__":
    sys.exit(main())
