<#
.SYNOPSIS
  Where each workflow runs. One rule, in one place, over every job of .github/workflows/.

.DESCRIPTION
  The rule (tasks/phase-3.6/01-built-at-home.md; README.md "Where CI runs"):

    everything that CHECKS runs at home    -> runs-on: [self-hosted, unraid]
    everything that SHIPS runs away        -> runs-on: a GitHub-hosted image

  At home is the organisation's self-hosted runners on Unraid (named unraid-1.., label `unraid`),
  because GitHub-hosted minutes got out of hand - twinoak/meshmaster.dk moved the same way on
  29 October 2026.

  Away is release.yml, all of it. It builds the images every repeater's update trusts and signs
  them with RELEASE_SIGNING_KEY (keys/README.md). The Unraid runner is shared with every branch a
  task session pushes and it keeps its home - ~/.platformio and its toolchains - from one job to
  the next, so what ships is built where each job begins new. Signing somewhere else would keep
  the key off the runner but would still sign whatever the runner built. release.yml runs at a
  tag, so its minutes are few.

  ci.yml runs this as a step, early, so neither half of the rule moves by accident. It is also a
  command:

    pwsh -File tools/ci/check-runners.ps1

  Exit code 0 when every job is where it belongs, 1 otherwise - and the failure names the file and
  the job. Runs on Windows PowerShell 5.1 and on pwsh under Linux.
#>
[CmdletBinding()]
param(
    # The repository root. Default: two directories up from this script.
    [string]$Root = ''
)

$ErrorActionPreference = 'Stop'
# Not a parameter default: $PSScriptRoot is not bound while the parameters are (5.1).
if (-not $Root) { $Root = Join-Path $PSScriptRoot '..\..' }
$Root = (Resolve-Path $Root).Path

# The one file that ships, and so the one file that stays on GitHub's runners.
$ships = 'release.yml'
# What "at home" is: exactly these two labels, in any order and nothing besides.
$atHome = @('self-hosted', 'unraid')
# A GitHub-hosted image is one of these three families (actions/runner-images). A job of
# release.yml may name any of them; `self-hosted` is never one.
$hostedPrefixes = @('ubuntu-', 'windows-', 'macos-')

function Get-Indent([string]$line) {
    if ($line -match '^(\s*)') { return $Matches[1].Length }
    return 0
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
            $value = $Matches[1]
            if ($value -eq '') {
                # The block-sequence form: one `- label` per line under it.
                $labels = @()
                for ($j = $i + 1; $j -lt $lines.Count; $j++) {
                    if ($lines[$j] -match '^\s*$') { continue }
                    elseif ($lines[$j] -match '^\s*-\s*(.+?)\s*$') { $labels += $Matches[1].Trim('"', "'") }
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

$workflowDir = Join-Path $Root '.github/workflows'
if (-not (Test-Path $workflowDir)) {
    Write-Host "check-runners: no $workflowDir"
    exit 1
}

$files = @(Get-ChildItem -LiteralPath $workflowDir -File |
    Where-Object { @('.yml', '.yaml') -contains $_.Extension } | Sort-Object Name)
if ($files.Count -eq 0) {
    Write-Host "check-runners: no workflow in $workflowDir"
    exit 1
}

$failures = @()
function Add-Failure([string]$file, [string]$job, [int]$line, [string]$message) {
    $where = ".github/workflows/$file"
    $named = if ($job) { " job '$job'" } else { '' }
    $script:failures += "$where${named}: $message"
    $loc = if ($line -gt 0) { ",line=$line" } else { '' }
    Write-Host "::error file=$where$loc::${where}${named}: $message"
}

Write-Host "check-runners: the rule is in this script - what CHECKS runs at home ([self-hosted, unraid]); what SHIPS ($ships) stays on a GitHub-hosted image"
foreach ($file in $files) {
    $jobs = @(Get-WorkflowJobs $file.FullName)
    if ($jobs.Count -eq 0) {
        Add-Failure $file.Name '' 0 "no job found - this guard could not read the file, so it cannot vouch for it"
        continue
    }
    $away = ($file.Name -eq $ships)
    foreach ($job in $jobs) {
        $labels = @($job.RunsOn)
        if ($labels.Count -eq 0) {
            if ($job.Uses) {
                Write-Host ("ok   {0,-14} {1,-30} calls a reusable workflow" -f $file.Name, $job.Job)
                continue
            }
            Add-Failure $file.Name $job.Job $job.Line 'no runs-on'
            continue
        }
        $shown = '[' + ($labels -join ', ') + ']'
        if ($away) {
            $hosted = @($hostedPrefixes | Where-Object { $labels[0].StartsWith($_) })
            if ($labels.Count -eq 1 -and $hosted.Count -gt 0) {
                Write-Host ("ok   {0,-14} {1,-30} {2} - away, because it ships" -f $file.Name, $job.Job, $shown)
            }
            else {
                Add-Failure $file.Name $job.Job $job.RunsOnAt ("runs-on is $shown; every job of $ships builds and signs what every repeater's update trusts, and stays on a GitHub-hosted image (ubuntu-*, windows-*, macos-*)")
            }
        }
        else {
            $extra = @($labels | Where-Object { $atHome -notcontains $_ })
            $missing = @($atHome | Where-Object { $labels -notcontains $_ })
            if ($extra.Count -eq 0 -and $missing.Count -eq 0) {
                Write-Host ("ok   {0,-14} {1,-30} {2} - at home" -f $file.Name, $job.Job, $shown)
            }
            else {
                Add-Failure $file.Name $job.Job $job.RunsOnAt ("runs-on is $shown; everything but $ships checks rather than ships and runs at home, on [self-hosted, unraid]")
            }
        }
    }
}

Write-Host ''
if ($failures.Count -gt 0) {
    Write-Host "check-runners: $($failures.Count) job(s) in the wrong place"
    exit 1
}
Write-Host 'check-runners: every job is where it belongs'
exit 0
