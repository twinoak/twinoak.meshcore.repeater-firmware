#!/usr/bin/env python3
"""Keep the copies of this specification honest.

The specification lives in twinoak.meshcore.standard-repeater-design-v2/software/spec. Each
firmware repository carries a copy under spec/, and a MANIFEST.txt beside it with the SHA-256 and
size of every file. This script writes the copy and checks it.

    python3 specsync.py copy <firmware-repo>     from the hardware repository: copy to <repo>/spec
                                                 and write <repo>/spec/MANIFEST.txt
    python3 specsync.py verify <spec-dir>        anywhere: do the files still match MANIFEST.txt?
                                                 (this is the line CI runs: ... verify spec)

Standard library only. A copy is never edited: change the specification where it lives, then copy.
"""

import hashlib
import os
import shutil
import subprocess
import sys

MANIFEST = "MANIFEST.txt"
# Files of a copy that belong to the repository holding it, not to the specification.
LOCAL = {MANIFEST, "README.md"}
SKIP_DIRS = {"__pycache__"}


def spec_files(root):
    out = []
    for base, dirs, files in os.walk(root):
        dirs[:] = sorted(d for d in dirs if d not in SKIP_DIRS)
        for name in sorted(files):
            rel = os.path.relpath(os.path.join(base, name), root).replace(os.sep, "/")
            if rel in LOCAL or rel == ".gitattributes" or name.endswith(".pyc"):
                continue
            out.append(rel)
    return sorted(out)


def digest(path):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        data = fh.read()
    h.update(data)
    return h.hexdigest(), len(data)


def source_commit(root):
    try:
        sha = subprocess.run(["git", "-C", root, "rev-parse", "HEAD"], capture_output=True, text=True, check=True)
        dirty = subprocess.run(["git", "-C", root, "status", "--porcelain", "--", "."], capture_output=True, text=True)
        return sha.stdout.strip() + (" plus uncommitted changes" if dirty.stdout.strip() else "")
    except Exception:
        return "unknown (not a git checkout)"


def write_manifest(spec_dir, source):
    lines = [
        "# The system specification, copied - never edited here.",
        "# source: twinoak.meshcore.standard-repeater-design-v2/software/spec at commit " + source,
        "# written by spec/tools/specsync.py; README.md in this directory says how",
        "#",
        "# sha256                                                           bytes  name",
    ]
    for rel in spec_files(spec_dir):
        sha, size = digest(os.path.join(spec_dir, rel))
        lines.append("%s  %7d  %s" % (sha, size, rel))
    with open(os.path.join(spec_dir, MANIFEST), "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(lines) + "\n")
    return len(lines) - 5


def verify(spec_dir):
    listed = {}
    with open(os.path.join(spec_dir, MANIFEST), "r", encoding="utf-8") as fh:
        for line in fh:
            line = line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            sha, size, rel = line.split(None, 2)
            listed[rel] = (sha, int(size))
    problems = []
    present = set(spec_files(spec_dir))
    for rel, (sha, size) in sorted(listed.items()):
        path = os.path.join(spec_dir, rel)
        if not os.path.isfile(path):
            problems.append("missing: " + rel)
            continue
        got_sha, got_size = digest(path)
        if got_size != size or got_sha != sha:
            problems.append("changed: %s (%d bytes %s, manifest says %d bytes %s)" % (rel, got_size, got_sha[:12], size, sha[:12]))
    for rel in sorted(present - set(listed)):
        problems.append("not in the manifest: " + rel)
    return len(listed), problems


def main(argv):
    here = os.path.dirname(os.path.abspath(__file__))
    if len(argv) == 3 and argv[1] == "copy":
        source = os.path.dirname(here)  # .../software/spec
        dest = os.path.join(argv[2], "spec")
        os.makedirs(dest, exist_ok=True)
        wanted = spec_files(source)
        for rel in spec_files(dest):
            if rel not in wanted:
                os.remove(os.path.join(dest, rel))
        for rel in wanted:
            target = os.path.join(dest, rel)
            os.makedirs(os.path.dirname(target), exist_ok=True)
            shutil.copyfile(os.path.join(source, rel), target)
        count = write_manifest(dest, source_commit(source))
        print("copied %d files to %s" % (count, dest))
        return 0
    if len(argv) == 3 and argv[1] == "verify":
        count, problems = verify(argv[2])
        for p in problems:
            print(p)
        print("%s: %d files, %s" % (argv[2], count, "all as the manifest says" if not problems else "%d PROBLEM(S)" % len(problems)))
        return 1 if problems else 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
