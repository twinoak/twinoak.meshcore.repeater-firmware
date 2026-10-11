<#
.SYNOPSIS
  Is there a MeshCore repeater tag newer than the pin?

.DESCRIPTION
  Tier one of docs\upstream.md, and the only part of it worth having outside a workflow:
  .github\workflows\upstream.yml runs this weekly and opens a pull request when the answer is
  yes. Having it here means the answer can be asked by hand, and the workflow understood, without
  waiting a week or pushing a branch:

    .\tools\upstream-check.ps1                                 # against the pin in platformio.ini
    .\tools\upstream-check.ps1 -Tag repeater-v1.16.0           # as if the pin were one tag back

  The tag family is repeater-v*, which is the one this firmware is built on: MeshCore tags a
  release per example family (repeater-v*, companion-v*, room-v*) and our [meshcore].tag is a
  repeater tag (spec/SYSTEM.md S-2, tasks/phase-0/reports/01.md).

  It reads and writes nothing of the repository. Moving the pin is the workflow's job, on a
  branch, and never this script's.

.PARAMETER Tag
  Pretend the repository is pinned at this tag. The "there is something newer" path without a
  real bump - what the acceptance check of tasks/phase-0/03-the-upstream-watch.md uses.

.PARAMETER Upstream
  MeshCore's git URL. Empty (the default) reads [meshcore].url out of platformio.ini, so that the
  one place this repository names upstream stays the one place.

.PARAMETER GitHubOutput
  Also append newer/tag/sha/previous_tag/previous_sha to $env:GITHUB_OUTPUT, for the workflow.
#>
[CmdletBinding()]
param(
    [string]$Tag = '',
    [string]$Upstream = '',
    [switch]$GitHubOutput
)

$ErrorActionPreference = 'Continue'   # native commands write to stderr; exit codes are checked by hand
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$ini = Join-Path $root 'platformio.ini'

$pin = ''
$url = ''
$currentTag = $Tag
foreach ($line in Get-Content $ini) {
    if ($line -match '^pin\s*=\s*([0-9a-f]{40})') { $pin = $Matches[1] }
    elseif ($line -match '^url\s*=\s*(\S+)') { $url = $Matches[1] }
    elseif (-not $Tag -and $line -match '^tag\s*=\s*(repeater-v\d+\.\d+\.\d+)') { $currentTag = $Matches[1] }
}
if (-not $pin) { throw "could not read [meshcore].pin from $ini" }
if ($currentTag -notmatch '^repeater-v(\d+\.\d+\.\d+)$') { throw "could not read [meshcore].tag from $ini" }
$currentVersion = [version]$Matches[1]
if (-not $Upstream) { $Upstream = $url }
if (-not $Upstream) { throw "could not read [meshcore].url from $ini" }

# Every repeater tag upstream has, newest by version and not by the order git prints them.
$refs = & git ls-remote --tags $Upstream 'refs/tags/repeater-v*' 2>$null
if ($LASTEXITCODE -ne 0) { throw "could not read the tags of $Upstream" }
$tags = @{}
foreach ($line in @($refs)) {
    # "<sha>\trefs/tags/repeater-v1.17.1" and, for an annotated tag, "...^{}" with the commit.
    if ($line -notmatch '^([0-9a-f]{40})\s+refs/tags/(repeater-v\d+\.\d+\.\d+)(\^\{\})?$') { continue }
    $sha = $Matches[1]; $name = $Matches[2]; $peeled = [bool]$Matches[3]
    # The peeled line is the commit and wins over the tag object. A pin has to be a commit:
    # MeshCore's repeater tags are annotated, so refs/tags/X is the tag object and X^{} is what
    # PlatformIO would check out.
    if ($peeled -or -not $tags.ContainsKey($name)) { $tags[$name] = $sha }
}
if ($tags.Count -eq 0) { throw "no repeater-v* tags at $Upstream" }

$newest = $tags.Keys | Sort-Object { [version]($_ -replace '^repeater-v', '') } | Select-Object -Last 1
$newestVersion = [version]($newest -replace '^repeater-v', '')
$newer = $newestVersion -gt $currentVersion

# What the commit list in a bump's pull request starts from. Normally the pin; under -Tag it is
# that tag's own commit, because a range from the real pin to a newer tag would be the empty one
# this rehearsal is pretending not to be in. (The workflow's rehearsal does not need this: it
# really rewinds the pin on a branch, so the file it reads already says the older commit.)
$previousSha = $pin
if ($Tag -and $tags.ContainsKey($Tag)) { $previousSha = $tags[$Tag] }

Write-Host "pinned:  $currentTag  $previousSha$(if ($Tag) { "  (pretended; platformio.ini says $pin)" })"
Write-Host "newest:  $newest  $($tags[$newest])"
if ($newer) { Write-Host "newer:   yes - $currentTag -> $newest" }
else { Write-Host "newer:   no - nothing to bump" }

if ($GitHubOutput -and $env:GITHUB_OUTPUT) {
    @(
        "newer=$($newer.ToString().ToLowerInvariant())",
        "tag=$newest",
        "sha=$($tags[$newest])",
        "previous_tag=$currentTag",
        "previous_sha=$previousSha"
    ) | Out-File -Append -Encoding utf8 $env:GITHUB_OUTPUT
}
