<#
.SYNOPSIS
  Where every job of every workflow runs. One rule, in one place, checked on every run.

.DESCRIPTION
    pwsh -File tools/ci/check-runners.ps1              every job of .github/workflows/
    pwsh -File tools/ci/check-runners.ps1 -SelfTest    the rule against planted workflows, then the above
    pwsh -File tools/ci/check-runners.ps1 -Dir <dir>   a directory of workflow files

  THE RULE. Every job of every workflow here runs on a GitHub-hosted runner. None of them runs on
  a self-hosted one: this repository is public, and a fork's pull request would run on the host.

    ci.yml, upstream.yml, release.yml, and any workflow added later    runs-on: ubuntu-latest

  WHY, at more length than one sentence. **This repository is public since 13 November 2026** -
  twinoak/meshmaster.dk is private, and so was this one until that afternoon - and that one fact
  gives both halves of the reason:

    Running here costs nothing. Actions on standard GitHub-hosted runners are free in a public
    repository (GitHub, *Billing and usage for Actions*: "GitHub Actions usage is free for
    standard GitHub-hosted runners in public repositories"), so this repository's CI spends no
    credit. The minutes that got out of hand in October 2026 are the private repositories', and
    their CI is the one that moved home to the organisation's Unraid runners
    (myoung34/github-runner, containers unraid-1..5, labels `self-hosted` and `unraid`).

    A self-hosted runner on a public repository runs a stranger's code. Anyone may fork this
    repository and open a pull request, and a pull request can change a workflow file; on a
    self-hosted runner that would execute on the Unraid host, with its docker socket, its LAN and
    its Tailscale. GitHub's own guidance (*Security hardening for GitHub Actions*): self-hosted
    runners "should almost never be used for public repositories". The organisation's one runner
    group is *All repositories, excluding public repositories*, which is what stops it from
    outside - and which is why no job of this repository was given a runner between 13 November
    2026 and this change. This guard is what stops it being worked around from inside.

  So there is no `release.yml` exception and no file that is special: one rule, every file. The
  half-and-half rule this replaced - *what CHECKS runs at home, what SHIPS runs away*
  (tasks/phase-3.6/01-built-at-home.md) - is in commit `4cb2200` with its own version of this
  script, ready to take back the day this repository is made private again; README's *Where CI
  runs* says so too. Its other argument is not lost and is in that section: a self-hosted runner
  is shared with every branch a task session pushes and keeps `~/.platformio` and its toolchains
  from one job to the next, so what ships should be built where each job begins new.

  WHAT IT REFUSES, and why each is a refusal rather than a pass: a `self-hosted` label (the rule
  itself); a label that is neither GitHub's nor `self-hosted` - a bare `unraid`, which reaches
  the same runners by its own label, or a misspelt `ubunut-latest`, which would queue for ever; a
  `runs-on` that is an expression, because where a job runs is not something to decide at run
  time and this guard cannot see what it evaluates to; a job with no `runs-on` at all; a workflow
  file this reader could find no job in, because a rule that could not be applied has not been
  passed. The one exception is a job whose work is a reusable workflow (`uses:` at the job
  level), which by YAML's rules has no `runs-on` of its own - the called workflow carries one,
  and this guard would check it if it were a file in this directory.

  The reader below understands the slice of YAML these workflows are written in: a `jobs:`
  mapping at column 0, one job per key two spaces in, and that job's `runs-on` two spaces
  further. It is block-scalar aware (a `run: |` body is skipped whole), so a heredoc cannot be
  misread as a job, and it fails loudly rather than quietly when a file is not in that shape.

  ci.yml runs this as a step, early - before PlatformIO is installed and before anything is built
  - so that the day someone moves a job home is the day a job goes red rather than the day a
  stranger's code runs on Unraid. Exit code 0 when every job is where it belongs, 1 otherwise,
  and a failure names the file, the job and the line. Runs on Windows PowerShell 5.1 and on pwsh
  under Linux.

  This is `tools/check-runners.py` of twinoak/twinoak.meshmaster.exitnode-firmware in PowerShell:
  that repository has been public longer, took this rule in its task p1.8-29, and its guard - its
  rule, its reasons, its refusals and its planted workflows - is the model for this one.
#>
[CmdletBinding()]
param(
    # The repository root. Default: two directories up from this script.
    [string]$Root = '',
    # A directory of workflow files to check instead of <root>/.github/workflows.
    [string]$Dir = '',
    # Run the rule against the planted workflows below first, then the real check.
    [switch]$SelfTest
)

$ErrorActionPreference = 'Stop'
# Not a parameter default: $PSScriptRoot is not bound while the parameters are (5.1).
# Forward slashes: this runs under pwsh on Linux in CI, where a backslash is an ordinary
# character in a file name and only the provider's normalisation saves it.
if (-not $Root) { $Root = Join-Path $PSScriptRoot '../..' }
$Root = (Resolve-Path $Root).Path

# The label GitHub puts on every self-hosted runner of its own accord, and so the one that names
# what must not happen here.
$SelfHostedLabel = 'self-hosted'

# GitHub's own runner images, as the label prefixes they are named with (actions/runner-images).
# Asking for one of these rather than for the absence of `self-hosted` is what catches a
# self-hosted runner addressed by its own label (`runs-on: unraid`) and a typo that would queue
# for ever ("ubunut-latest"). `ubuntu-latest` moves to Ubuntu 26 under us one day; the prefix
# takes a pinned `ubuntu-24.04` unchanged.
$HostedPrefixes = @('ubuntu-', 'windows-', 'macos-', 'macOS-')

# One sentence, in one place, so that both halves of the why travel together wherever a refusal is
# printed.
$Reason = 'a public repository; a fork''s pull request would run on the host. Every job here ' +
          'runs on a GitHub-hosted runner, which is free for a public repository.'

function Get-Indent([string]$line) {
    if ($line -match '^(\s*)') { return $Matches[1].Length }
    return 0
}

function Remove-Comment([string]$value) {
    # Enough for a runs-on: a `#` at the start or after whitespace ends the value.
    if ($value -match '^(.*?)(?:(?<=\s)|^)#.*$') { return $Matches[1].Trim() }
    return $value.Trim()
}

function Get-RunsOnTokens([string]$value) {
    # `[a, b]` -> a, b; `ubuntu-latest` -> ubuntu-latest; a quoted scalar unquoted.
    $v = $value.Trim()
    if ($v -match '^\[(.*)\]$') {
        return @($Matches[1] -split ',' | ForEach-Object { $_.Trim().Trim('"', "'") } | Where-Object { $_ })
    }
    return @($v.Trim('"', "'"))
}

function Get-WorkflowJobs([string]$path) {
    # Every job of one workflow file, with the line its runs-on is on. A line-based read, not a
    # YAML parse: enough for these files, and it skips the body of a block scalar (a `run: |`) so
    # that a shell line which happens to say runs-on is not mistaken for a job's.
    $jobs = @()
    $lines = @(Get-Content -LiteralPath $path)
    $inJobs = $false
    $current = $null
    $blockIndent = -1
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $line = $lines[$i]
        if ($line -match '^\s*$') { continue }
        if ($blockIndent -ge 0) {
            if ((Get-Indent $line) -gt $blockIndent) { continue }
            $blockIndent = -1
        }
        if ($line -match '^(\s*)-?\s*[A-Za-z0-9_.\-]+:\s*[|>][-+0-9]*\s*$') {
            $blockIndent = $Matches[1].Length
            continue
        }
        if ($line -match '^\s*#') { continue }
        if ($line -match '^jobs:\s*$') { $inJobs = $true; continue }
        if ($inJobs -and $line -match '^[^\s]') { $inJobs = $false; $current = $null; continue }
        if (-not $inJobs) { continue }
        if ($line -match '^  ([A-Za-z0-9_.\-]+):\s*$') {
            $current = [pscustomobject]@{
                Job      = $Matches[1]
                Line     = $i + 1
                RunsOn   = @()
                RunsOnAt = 0
                Uses     = $false
            }
            $jobs += $current
            continue
        }
        if (-not $current) { continue }
        if ($line -match '^    runs-on:\s*(.*?)\s*$') {
            $current.RunsOnAt = $i + 1
            $value = Remove-Comment $Matches[1]
            if ($value -eq '') {
                # The block-sequence form: one `- label` per line under it.
                $labels = @()
                for ($j = $i + 1; $j -lt $lines.Count; $j++) {
                    if ($lines[$j] -match '^\s*$') { continue }
                    elseif ($lines[$j] -match '^\s*#') { continue }
                    elseif ($lines[$j] -match '^\s*-\s*(.+?)\s*$') { $labels += (Remove-Comment $Matches[1]).Trim('"', "'") }
                    else { break }
                }
                $current.RunsOn = $labels
            }
            else {
                $current.RunsOn = Get-RunsOnTokens $value
            }
            continue
        }
        if ($line -match '^    uses:\s*\S') { $current.Uses = $true }
    }
    return $jobs
}

function Invoke-Check {
    # $Annotate = $false for the self-test: its refusals are the point rather than a problem, and
    # an `::error::` line is a workflow command that GitHub turns into a *failure annotation*
    # against the file and the line it names. The planted files are called ci.yml, release.yml and
    # upstream.yml too, so a green run would otherwise carry a dozen annotations pointing at real
    # lines of this repository that are not wrong at all (the exit-node firmware found that on its
    # run 41191528050, task p1.8-29).
    param([string]$Directory, [bool]$Annotate = $true)

    $failures = @()

    if (-not (Test-Path $Directory)) {
        Write-Host "check-runners: no $Directory"
        return 1
    }
    $files = @(Get-ChildItem -LiteralPath $Directory -File |
        Where-Object { @('.yml', '.yaml') -contains $_.Extension } | Sort-Object Name)
    if ($files.Count -eq 0) {
        Write-Host "check-runners: $Directory holds no workflow file, so nothing was checked"
        return 1
    }

    foreach ($file in $files) {
        $jobs = @(Get-WorkflowJobs $file.FullName)
        $refusals = @()
        if ($jobs.Count -eq 0) {
            $refusals += [pscustomobject]@{ Job = ''; Line = 0; Message = ('no job found - this ' +
                'guard could not read the file, so it cannot vouch for it (is `jobs:` at column 0?)') }
        }
        foreach ($job in $jobs) {
            $labels = @($job.RunsOn)
            if ($labels.Count -eq 0) {
                if ($job.Uses) {
                    Write-Host ("ok   {0,-14} {1,-30} calls a reusable workflow" -f $file.Name, $job.Job)
                    continue
                }
                $refusals += [pscustomobject]@{ Job = $job.Job; Line = $job.Line; Message = 'no runs-on' }
                continue
            }
            $shown = '[' + ($labels -join ', ') + ']'
            if (($labels -join ' ') -match '\$\{\{') {
                $refusals += [pscustomobject]@{ Job = $job.Job; Line = $job.RunsOnAt; Message =
                    ("runs-on is $shown, an expression. Where a job runs is not something to " +
                     'decide at run time - an expression can evaluate to a self-hosted runner and ' +
                     'this guard cannot see that it does not: write the labels.') }
                continue
            }
            if ($labels -contains $SelfHostedLabel) {
                $refusals += [pscustomobject]@{ Job = $job.Job; Line = $job.RunsOnAt; Message =
                    ("runs-on is $shown, a self-hosted runner. No job here may run on one - $Reason") }
                continue
            }
            $hosted = @($HostedPrefixes | Where-Object { $labels[0].StartsWith($_) })
            if ($labels.Count -eq 1 -and $hosted.Count -gt 0) {
                Write-Host ("ok   {0,-14} {1,-30} {2}" -f $file.Name, $job.Job, $shown)
            }
            else {
                $refusals += [pscustomobject]@{ Job = $job.Job; Line = $job.RunsOnAt; Message =
                    ("runs-on is $shown, which is not a GitHub-hosted runner label (one of " +
                     '`ubuntu-*`, `windows-*`, `macos-*`). If it is a self-hosted runner addressed ' +
                     "by its own label, the rule is the same - $Reason") }
            }
        }
        foreach ($refusal in $refusals) {
            $where = ".github/workflows/$($file.Name)"
            $named = ''
            if ($refusal.Job) { $named = " job '$($refusal.Job)'" }
            $failures += "$where${named}: $($refusal.Message)"
            if ($Annotate) {
                $loc = ''
                if ($refusal.Line -gt 0) { $loc = ",line=$($refusal.Line)" }
                Write-Host "::error file=$where$loc::${where}${named}: $($refusal.Message)"
            }
            else {
                Write-Host "  refused: $where${named}: $($refusal.Message)"
            }
        }
    }

    if ($failures.Count -gt 0) {
        Write-Host ''
        Write-Host ("check-runners: $($failures.Count) job(s) run in the wrong place. The rule, and " +
            'why it is the rule, is at the top of tools/ci/check-runners.ps1')
        return 1
    }
    Write-Host ("check-runners: every job of $($files.Count) workflow(s) runs on a GitHub-hosted " +
        "runner - free for a public repository, and no fork's pull request on the Unraid host")
    return 0
}

# ---- The planted workflows: the shapes the reader must still read, and what the rule must refuse.

$HostedLine = 'runs-on: ubuntu-latest'

$GoodCi = @'
name: ci
on: [push]
jobs:
  verify:
    name: verify
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - name: A heredoc that must not be read as a job
        run: |
          cat <<'XEOFX'
          native:
            runs-on: [self-hosted, unraid]
          XEOFX
  second:
    runs-on:
      - ubuntu-latest
    steps:
      - run: echo hi
'@

$GoodRelease = @'
name: release
on:
  workflow_dispatch:
jobs:
  ci-green:
    runs-on: ubuntu-latest
    steps:
      - run: echo hi
'@

$GoodUpstream = @'
name: upstream
on:
  schedule:
    - cron: "29 6 * * 1"
jobs:
  bump:
    runs-on: ubuntu-latest
    steps:
      - run: echo hi
'@

$ReusableCi = @'
name: ci
on: [push]
jobs:
  verify:
    uses: ./.github/workflows/called.yml
'@

# The repository is `* text=auto eol=lf` (.gitattributes), so these here-strings are LF - but the
# cases below cut two lines out of one with a "`n" of their own, and a checkout that ever arrived
# with CRLF would make those replacements miss silently rather than fail.
$GoodCi = $GoodCi -replace "`r`n", "`n"
$GoodRelease = $GoodRelease -replace "`r`n", "`n"
$GoodUpstream = $GoodUpstream -replace "`r`n", "`n"
$ReusableCi = $ReusableCi -replace "`r`n", "`n"

function New-SelfTestCase([string]$Label, [hashtable]$Files, [int]$Expected) {
    return [pscustomobject]@{ Label = $Label; Files = $Files; Expected = $Expected }
}

function Get-SelfTestCases {
    # One case per shape, each with the three files this repository has, so that a case says one
    # thing. The expected exit code is 0 for what must be accepted and 1 for what must be refused.
    $cases = @()
    $cases += New-SelfTestCase 'what this repository has' @{
        'ci.yml' = $GoodCi; 'release.yml' = $GoodRelease; 'upstream.yml' = $GoodUpstream } 0
    $cases += New-SelfTestCase "ci.yml's verify at home" @{
        'ci.yml' = $GoodCi.Replace($HostedLine, 'runs-on: [self-hosted, unraid]')
        'release.yml' = $GoodRelease; 'upstream.yml' = $GoodUpstream } 1
    $cases += New-SelfTestCase 'self-hosted with any other label, not only unraid' @{
        'ci.yml' = $GoodCi.Replace($HostedLine, 'runs-on: [self-hosted, linux, x64]')
        'release.yml' = $GoodRelease; 'upstream.yml' = $GoodUpstream } 1
    $cases += New-SelfTestCase 'self-hosted on its own' @{
        'ci.yml' = $GoodCi.Replace($HostedLine, 'runs-on: self-hosted')
        'release.yml' = $GoodRelease; 'upstream.yml' = $GoodUpstream } 1
    $cases += New-SelfTestCase "a runner group's own label, with no self-hosted beside it" @{
        'ci.yml' = $GoodCi.Replace($HostedLine, 'runs-on: unraid')
        'release.yml' = $GoodRelease; 'upstream.yml' = $GoodUpstream } 1
    $cases += New-SelfTestCase 'release.yml at home, which is no different' @{
        'ci.yml' = $GoodCi; 'upstream.yml' = $GoodUpstream
        'release.yml' = $GoodRelease.Replace($HostedLine, 'runs-on: [self-hosted, unraid]') } 1
    $cases += New-SelfTestCase 'upstream.yml at home, which is no different either' @{
        'ci.yml' = $GoodCi; 'release.yml' = $GoodRelease
        'upstream.yml' = $GoodUpstream.Replace($HostedLine, 'runs-on: [self-hosted, unraid]') } 1
    $cases += New-SelfTestCase 'the block-sequence form, at home' @{
        'ci.yml' = $GoodCi.Replace("    runs-on:`n      - ubuntu-latest",
                                   "    runs-on:`n      - self-hosted`n      - unraid")
        'release.yml' = $GoodRelease; 'upstream.yml' = $GoodUpstream } 1
    $cases += New-SelfTestCase 'a job with no runs-on' @{
        'ci.yml' = $GoodCi.Replace("    runs-on:`n      - ubuntu-latest`n", '')
        'release.yml' = $GoodRelease; 'upstream.yml' = $GoodUpstream } 1
    $cases += New-SelfTestCase 'a runs-on decided at run time' @{
        'ci.yml' = $GoodCi.Replace($HostedLine, 'runs-on: ${{ vars.RUNNER }}')
        'release.yml' = $GoodRelease; 'upstream.yml' = $GoodUpstream } 1
    $cases += New-SelfTestCase 'a label that looks hosted and is a typo' @{
        'ci.yml' = $GoodCi; 'upstream.yml' = $GoodUpstream
        'release.yml' = $GoodRelease.Replace($HostedLine, 'runs-on: ubunut-latest') } 1
    $cases += New-SelfTestCase 'a workflow with no job at all' @{
        'ci.yml' = $GoodCi; 'release.yml' = $GoodRelease; 'upstream.yml' = $GoodUpstream
        'stray.yml' = "name: stray`non: [push]`n" } 1
    $cases += New-SelfTestCase 'a job whose work is a reusable workflow' @{
        'ci.yml' = $ReusableCi; 'release.yml' = $GoodRelease; 'upstream.yml' = $GoodUpstream } 0
    $cases += New-SelfTestCase 'a trailing comment on a hosted runs-on' @{
        'ci.yml' = $GoodCi.Replace($HostedLine, 'runs-on: ubuntu-latest  # away, like every job here')
        'release.yml' = $GoodRelease; 'upstream.yml' = $GoodUpstream } 0
    $cases += New-SelfTestCase 'a directory with no workflow in it' @{ } 1
    return $cases
}

function Invoke-SelfTest {
    $cases = @(Get-SelfTestCases)
    $verdicts = @()
    $bad = 0
    foreach ($case in $cases) {
        $dir = Join-Path ([System.IO.Path]::GetTempPath()) ('check-runners-' + [guid]::NewGuid().ToString('N'))
        New-Item -ItemType Directory -Force -Path $dir | Out-Null
        try {
            foreach ($name in @($case.Files.Keys)) {
                Set-Content -LiteralPath (Join-Path $dir $name) -Value $case.Files[$name] -Encoding UTF8
            }
            Write-Host "--- self-test: $($case.Label)"
            $got = Invoke-Check $dir $false
        }
        finally {
            Remove-Item -LiteralPath $dir -Recurse -Force -ErrorAction SilentlyContinue
        }
        $verdict = 'ok'
        if ($got -ne $case.Expected) { $verdict = 'WRONG'; $bad++ }
        $verdicts += ("self-test {0,-5} {1,-55} expected {2}, got {3}" -f
            $verdict, $case.Label, $case.Expected, $got)
    }
    Write-Host ''
    foreach ($line in $verdicts) { Write-Host $line }
    if ($bad -gt 0) {
        Write-Host ''
        Write-Host "check-runners: $bad self-test case(s) wrong"
        return 1
    }
    Write-Host "self-test: $($cases.Count) cases, all as expected"
    return 0
}

# ---- What a run of this script does.

Write-Host ('check-runners: the rule is in this script - every job of every workflow runs on a ' +
    'GitHub-hosted runner, because this repository is public')

if ($SelfTest) {
    $code = Invoke-SelfTest
    if ($code -ne 0) { exit $code }
    Write-Host ''
}

$workflowDir = $Dir
if (-not $workflowDir) { $workflowDir = Join-Path $Root '.github/workflows' }
exit (Invoke-Check $workflowDir $true)
