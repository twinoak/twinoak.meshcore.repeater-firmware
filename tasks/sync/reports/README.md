# Reports

One per sync task, written by the session that did it and committed on
`upstream/<tag>` — the branch `.github/workflows/upstream.yml` pushed, not a
`task/...` branch of its own: `NN.md`, in the shape `tasks/RUNBOOK.md` §3
gives. It is the pull request's description (the pull request is already open,
so it is `gh pr edit --body-file` rather than `gh pr create`) and what the
reviewer reads.

`tasks/run-all.ps1 -Phase sync` considers a task done when `reports/NN.md` is
on `origin/main` and nothing else counts, so an unfinished report belongs in
`tasks/sync/logs/` (git-ignored) until it is one.

What a sync report has that others do not: the eight places of
[`docs/upstream.md`](../../../docs/upstream.md), each one *unchanged* or
*changed: was X, is now Y*.
