# MeshCore sync tasks

Tier two of keeping up with MeshCore. Tier one is automatic — a weekly workflow
opens a pull request that moves the pin, and a green one is merged by a person —
and this is what happens when that pull request is **red**:
`tools/new-sync-task.ps1 -Tag repeater-vX.Y.Z` writes a task file here from
[`TEMPLATE.md`](TEMPLATE.md) with the tag, the pull request and the failing
job's log in it, and the driver runs it like any other task:

```powershell
.\tools\new-sync-task.ps1 -Tag repeater-v1.18.0 -Pr 12 -LogUrl <the failing job>
.\tasks\run-all.ps1 -Phase sync
```

[`docs/upstream.md`](../../docs/upstream.md) is the page that explains all three
tiers and holds the list of the eight places a bump re-reads;
`TEMPLATE.md` is that list as a session's checklist. Reports land in
[`reports/NN.md`](reports/README.md) as everywhere else.

A sync task is the one kind of task that does **not** branch from `main`: the
branch `upstream/<tag>` is already on `origin`, pushed by the workflow with the
pin moved on it, and the session continues it (`tasks/RUNBOOK.md`, the
phase-specific note at the end). The driver knows: `-Phase sync` reads this
directory and never deletes that branch.

## Order and dependencies

Newest first. Each one is independent — a bump is a bump — but they are
numbered in the order they were written, and the driver runs them in that
order.

<!-- new-sync-task.ps1 inserts its row directly under the |---| line, in the
     driver's format: | NN | [name](NN-name.md) | tag | what happened | -->

| # | Task | Tag | Why it needed a session |
|---|---|---|---|

Empty: MeshCore has published no `repeater-v*` release since the pin was taken,
so there has not been a bump at all yet — let alone a red one.
