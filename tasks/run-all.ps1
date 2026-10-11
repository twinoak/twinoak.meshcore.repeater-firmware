<#
.SYNOPSIS
  Runs the tasks of a phase unattended: one fresh headless Claude Code session per task, in order.

.DESCRIPTION
  twinoak/meshmaster-agent-firmware's driver, on `main` instead of `master`. For each task of
  tasks\phase-<Phase>\README.md (its "Order and dependencies" table) from -From to -To: checks it
  has not landed yet (its report on origin/<trunk>), starts
  `claude -p "Run task NN of phase P ..." --dangerously-skip-permissions` in the repo, waits, and
  verifies the report is now on origin/<trunk>. The first task that does not land stops the run and
  writes tasks\phase-<Phase>\logs\STOPPED.md; fix or read the session log, then resume with -From NN.

  What each session does is in tasks\RUNBOOK.md. Session output goes to
  tasks\phase-<Phase>\logs\NN.log (git-ignored).

  The model: a task file may say `**Model:** <name>` in its header (e.g. claude-opus-5,
  claude-fable-5-1); otherwise -Model applies; otherwise Claude Code's own default.

  `-Phase sync` runs the MeshCore sync tasks of tasks\sync\ (docs\upstream.md): the same driver,
  a phase whose task files are written by tools\new-sync-task.ps1 rather than by hand.

  One-time setup: PlatformIO Core on PATH (`pip install platformio`, or the pio bundled with the
  VS Code extension - the preflight finds that one by itself). gh installed and logged in
  (winget install GitHub.cli; gh auth login) so sessions can open PRs and merge after CI -
  without it they merge locally after verification. A C++ compiler for `pio test -e native`:
  g++ on Linux, and on Windows either a MinGW g++ on PATH or Docker, which is how a session runs
  the host tests there (RUNBOOK.md section 2).

  .\tasks\run-all.ps1 -Phase 1                     # every task of phase 1 not yet on the trunk
  .\tasks\run-all.ps1 -Phase 1 -From 03            # resume
  .\tasks\run-all.ps1 -Phase 1 -From 04 -To 04     # one task
  .\tasks\run-all.ps1 -Phase 1 -Model claude-opus-5   # default model for tasks that name none
  .\tasks\run-pile.ps1                             # phase after phase, stopping at the first stop

  A task's branch is deleted - locally and on origin - once its report is on origin/<trunk> and
  origin/<trunk> contains the branch. The session is asked to do it first (RUNBOOK.md section 4);
  this is the second ask, for the session that was cut off before it got there.

  A session that dies on a usage or rate limit (Claude Code prints "usage limit" / "rate limit" and
  ends without landing the task) is not a stop: the driver waits -RetryWaitMinutes and starts the
  same task again, up to -MaxRetries times. The new session finds the task's branch and continues
  it (RUNBOOK.md section 1), so nothing done before the limit is lost. A deliberate stop (the
  session wrote STOPPED-NN.md) is never retried.

  The script ends with exit code 0 when every task of the range landed and 1 when it stopped, so
  that run-pile.ps1 can chain phases.
#>
[CmdletBinding()]
param(
    [string]$Phase = '1',
    [string]$From = '',
    [string]$To = '',
    [string]$Model = '',
    [int]$RetryWaitMinutes = 65,   # after a session died on a usage or rate limit: wait, then run the same task again
    [int]$MaxRetries = 8,          # ... this many times (8 x 65 min covers a five-hour window with room), then stop
    [string]$Trunk = 'main',       # the branch tasks branch from and merge to
    [switch]$SkipPreflight
)

$ErrorActionPreference = 'Continue'   # native commands write to stderr; exit codes are checked by hand
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$phaseDir = Join-Path $root "tasks\phase-$Phase"
if ($Phase -eq 'sync') { $phaseDir = Join-Path $root 'tasks\sync' }
$logs = Join-Path $phaseDir 'logs'
New-Item -ItemType Directory -Force -Path $logs | Out-Null
$runLog = Join-Path $logs 'run-all.log'

function Log([string]$m) {
    $line = "[{0}] {1}" -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'), $m
    Write-Host $line
    $line | Out-File -Append -Encoding utf8 $runLog
}
function Stop-Run([string]$why) {
    $why | Out-File -Encoding utf8 (Join-Path $logs 'STOPPED.md')
    Log "STOPPED: $why"
    exit 1
}
function Quiet([scriptblock]$cmd) {
    # run a native command, swallow its output, return the exit code
    & $cmd 2>&1 | Out-Null
    return $LASTEXITCODE
}
function Report-Landed([string]$n) {
    [void](Quiet { git fetch -q origin })
    $path = if ($Phase -eq 'sync') { "tasks/sync/reports/$n.md" } else { "tasks/phase-$Phase/reports/$n.md" }
    return (Quiet { git cat-file -e "origin/${Trunk}:$path" }) -eq 0
}
function Remove-Merged-Branch([string]$n, [string]$name) {
    # The task's branch, once its report is on origin/$Trunk. RUNBOOK.md section 4 asks the
    # session to delete it (`gh pr merge --delete-branch`, or `git branch -d` without gh), and a
    # session that was cut off between the merge and the delete leaves it behind - phase 1 left
    # five. So the driver asks again here, for this task's branch and no other.
    #
    # A branch is deleted only when origin/$Trunk already contains it: an unmerged branch is work
    # nobody has, and the one thing worse than a stale branch is a lost one. `upstream/<tag>` of
    # the sync phase is not the driver's to touch - the workflow pushed it (docs\upstream.md).
    if ($Phase -eq 'sync') { return }
    $branch = "task/p$Phase-$n-$name"
    [void](Quiet { git fetch -q origin })
    if ((Quiet { git rev-parse -q --verify "refs/remotes/origin/$branch" }) -eq 0) {
        if ((Quiet { git merge-base --is-ancestor "refs/remotes/origin/$branch" "refs/remotes/origin/$Trunk" }) -eq 0) {
            if ((Quiet { git push -q origin --delete $branch }) -eq 0) { Log "deleted origin/$branch (merged)" }
            else { Log "could not delete origin/$branch - delete it by hand" }
        }
        else {
            Log "origin/$branch is not in origin/$Trunk and stays: the report landed, the branch has more on it"
        }
    }
    if ((Quiet { git rev-parse -q --verify "refs/heads/$branch" }) -eq 0) {
        if ((Quiet { git merge-base --is-ancestor "refs/heads/$branch" "refs/remotes/origin/$Trunk" }) -eq 0) {
            [void](Quiet { git checkout $Trunk })
            if ((Quiet { git branch -q -d $branch }) -eq 0) { Log "deleted the local $branch (merged)" }
            else { Log "could not delete the local $branch - delete it by hand" }
        }
        else { Log "the local $branch is not in origin/$Trunk and stays" }
    }
}
function Commit-Notes([string]$label) {
    [void](Quiet { git checkout $Trunk })
    [void](Quiet { git pull --ff-only })
    if (git status --porcelain) {
        [void](Quiet { git add -A })
        [void](Quiet { git commit -q -m "Notes from Claude ($label)" })
        if ((Quiet { git push -q }) -ne 0) { Stop-Run "could not push the notes commit ($label)" }
        Log "committed and pushed notes ($label)"
    }
}

# The task list is the README's table: "| NN | [name](NN-name.md) | ...".
$readme = Join-Path $phaseDir 'README.md'
if (-not (Test-Path $readme)) { Stop-Run "no $readme" }
$tasks = @()
foreach ($line in Get-Content $readme) {
    if ($line -match '^\| (\d\d) \| \[([a-z0-9-]+)\]\(') { $tasks += @{ N = $Matches[1]; Name = $Matches[2] } }
}
if ($tasks.Count -eq 0) { Stop-Run "no task rows found in $readme" }
if (-not $From) { $From = $tasks[0].N }
if (-not $To) { $To = $tasks[-1].N }

Set-Location $root
Remove-Item (Join-Path $logs 'STOPPED.md') -ErrorAction SilentlyContinue
Log "run-all: phase $Phase, tasks $From..$To in $root"

if (-not $SkipPreflight) {
    # PlatformIO is often installed but not on PATH - the VS Code extension keeps its own copy in
    # ~\.platformio\penv. Finding it here means the sessions inherit it.
    if (-not (Get-Command pio -ErrorAction SilentlyContinue)) {
        foreach ($bundled in @((Join-Path $HOME '.platformio\penv\Scripts'), (Join-Path $HOME '.platformio/penv/bin'))) {
            if ((Test-Path (Join-Path $bundled 'pio.exe')) -or (Test-Path (Join-Path $bundled 'pio'))) {
                $env:PATH = "$bundled$([IO.Path]::PathSeparator)$env:PATH"
                Log "pio: not on PATH, using the one in $bundled"
                break
            }
        }
    }
    foreach ($tool in 'git', 'pio', 'claude') {
        if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) { Stop-Run "$tool is not on PATH" }
    }
    Log ("pio: " + ((& pio --version) -join ' '))
    if (Get-Command gh -ErrorAction SilentlyContinue) {
        if ((Quiet { gh auth status }) -ne 0) { Stop-Run "gh is installed but not logged in - run: gh auth login" }
        Log "gh: sessions open PRs and merge after CI is green"
    }
    else {
        Log "gh not found: sessions merge locally after verification (RUNBOOK.md section 4)"
    }
    # `pio test -e native` is a host build: it needs a C++ compiler, which a Windows box usually
    # has only inside a container. Not a stop - the session falls back to Docker and says so.
    $cxx = @('g++', 'clang++') | Where-Object { Get-Command $_ -ErrorAction SilentlyContinue } | Select-Object -First 1
    if ($cxx) { Log "native tests: $cxx" }
    elseif ((Quiet { docker version }) -eq 0) { Log "native tests: no g++ on PATH - sessions run them in a container (RUNBOOK.md section 2)" }
    else { Log 'WARNING: no g++ and no Docker - pio test -e native cannot run, and that will stop every task' }
}

if ((Quiet { git checkout $Trunk }) -ne 0) { Stop-Run "git checkout $Trunk failed" }
if ((Quiet { git pull --ff-only }) -ne 0) { Stop-Run "git pull --ff-only failed on $Trunk" }
Commit-Notes "pre-run"

foreach ($t in $tasks) {
    if ([int]$t.N -lt [int]$From -or [int]$t.N -gt [int]$To) { continue }
    $n = $t.N
    if (Report-Landed $n) {
        Log "task $n already on origin/$Trunk - skipping"
        Remove-Merged-Branch $n $t.Name
        continue
    }

    # Review notes the orchestrator wrote to the working tree between tasks land on the trunk first.
    Commit-Notes "before task $n"

    # The model: the task file's **Model:** line, else -Model, else Claude Code's default.
    $taskModel = $Model
    $taskFile = Join-Path $phaseDir "$n-$($t.Name).md"
    if (Test-Path $taskFile) {
        $header = Get-Content $taskFile -TotalCount 12 | Out-String
        if ($header -match '\*\*Model:\*\*\s*([A-Za-z0-9.\-]+)') { $taskModel = $Matches[1] }
    }

    $prompt = "Run task $n ($($t.Name)) of phase ${Phase}: follow tasks/RUNBOOK.md exactly, for this task only."
    $claudeArgs = @('-p', $prompt, '--dangerously-skip-permissions')
    if ($taskModel) { $claudeArgs += @('--model', $taskModel) }
    $stopped = Join-Path $logs "STOPPED-$n.md"
    Remove-Item $stopped -ErrorAction SilentlyContinue

    for ($attempt = 1; $attempt -le (1 + $MaxRetries); $attempt++) {
        $sessionLog = Join-Path $logs "$n.log"
        if ($attempt -gt 1) { $sessionLog = Join-Path $logs "$n.retry$($attempt - 1).log" }
        Log "task $n $($t.Name): starting headless session$(if ($taskModel) { " (model $taskModel)" })$(if ($attempt -gt 1) { " - attempt $attempt" })"
        $sw = [Diagnostics.Stopwatch]::StartNew()
        & claude @claudeArgs 2>&1 | ForEach-Object {
            if ($_ -is [System.Management.Automation.ErrorRecord]) { $_.Exception.Message } else { $_ }
        } | Tee-Object -FilePath $sessionLog
        $exit = $LASTEXITCODE
        Log ("task $n session ended (exit $exit) after {0} min" -f [int]$sw.Elapsed.TotalMinutes)

        [void](Quiet { git checkout $Trunk })
        [void](Quiet { git pull --ff-only })
        if (Report-Landed $n) { Remove-Merged-Branch $n $t.Name; break }

        if (Test-Path $stopped) {
            Stop-Run "task $n stopped on purpose: the session wrote $stopped. Resume with -Phase $Phase -From $n once fixed."
        }
        $tail = ''
        if (Test-Path $sessionLog) { $tail = (Get-Content $sessionLog -Raw -ErrorAction SilentlyContinue) -replace '\s+', ' ' }
        if ($tail.Length -gt 4000) { $tail = $tail.Substring($tail.Length - 4000) }
        $limited = $tail -match 'usage limit|rate limit|limit will reset|limit reached|overloaded|out of (tokens|credits)|529'
        if (-not $limited) {
            Stop-Run "task $n did not land on origin/$Trunk (no report). See $sessionLog. Resume with -Phase $Phase -From $n once fixed."
        }
        if ($attempt -gt $MaxRetries) {
            Stop-Run "task $n hit a usage or rate limit $MaxRetries times in a row (last: $sessionLog). Resume with -Phase $Phase -From $n when the limit has reset; the branch keeps the work done so far."
        }
        Log "task $n the session ended on a usage or rate limit; waiting $RetryWaitMinutes min, then continuing the same task (attempt $($attempt + 1) of $(1 + $MaxRetries))"
        Start-Sleep -Seconds (60 * $RetryWaitMinutes)
    }
    Log "task $n merged"
}

Commit-Notes "after task $To"
Log "Phase $Phase run complete for tasks $From..$To."
exit 0
