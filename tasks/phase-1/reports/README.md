# Reports

One per task, written by the session that did it and committed on its branch:
`NN.md`, in the shape `tasks/RUNBOOK.md` section 3 gives. It is the pull
request's description and what the reviewer reads.

`tasks/run-all.ps1` considers a task done when `reports/NN.md` is on
`origin/main` and nothing else counts, so an unfinished report belongs in
`tasks/phase-1/logs/` (git-ignored) until it is one.
