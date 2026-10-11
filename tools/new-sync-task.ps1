<#
.SYNOPSIS
  Writes a MeshCore sync task from tasks\sync\TEMPLATE.md.

.DESCRIPTION
  Tier two of docs\upstream.md. .github\workflows\upstream.yml opens a pull request that moves the
  pin to a new repeater-vX.Y.Z tag; when CI on it is red, that is the case that needs a session,
  and this is the one command that makes one:

    .\tools\new-sync-task.ps1 -Tag repeater-v1.18.0 -Pr 12
    .\tasks\run-all.ps1 -Phase sync

  It resolves the tag to its commit, reads the old pin out of platformio.ini, fills the template,
  writes tasks\sync\NN-meshcore-<tag>.md and inserts the row the driver's table needs. It changes
  nothing else: the pin is moved by the branch the workflow pushed, not by this script, and the
  session works on that branch.

.PARAMETER Tag
  The MeshCore tag to bump to, e.g. repeater-v1.18.0.

.PARAMETER Pr
  The pull request number upstream.yml opened, if there is one. Without it the task file says so.

.PARAMETER LogUrl
  A link to the failing job, or a few lines of it. Whatever a session should read first.

.PARAMETER Commit
  The tag's commit, when it should not be resolved over the network (a test, an air-gapped run).

.PARAMETER Upstream
  MeshCore's git URL. Empty (the default) reads [meshcore].url out of platformio.ini.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Tag,
    [int]$Pr = 0,
    [string]$LogUrl = '',
    [string]$Commit = '',
    [string]$Upstream = ''
)

$ErrorActionPreference = 'Continue'   # native commands write to stderr; exit codes are checked by hand
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$syncDir = Join-Path $root 'tasks\sync'
$template = Join-Path $syncDir 'TEMPLATE.md'
$readme = Join-Path $syncDir 'README.md'
$ini = Join-Path $root 'platformio.ini'

if ($Tag -notmatch '^repeater-v\d+\.\d+\.\d+$') { throw "not a MeshCore repeater tag: $Tag" }
if (-not (Test-Path $template)) { throw "no $template" }
if (-not (Test-Path $readme)) { throw "no $readme" }

# The pin this repository is on now, and where upstream is.
$oldPin = ''
$url = ''
foreach ($line in Get-Content $ini) {
    if ($line -match '^pin\s*=\s*([0-9a-f]{40})') { $oldPin = $Matches[1] }
    elseif ($line -match '^url\s*=\s*(\S+)') { $url = $Matches[1] }
}
if (-not $oldPin) { throw "could not read [meshcore].pin from $ini" }
if (-not $Upstream) { $Upstream = $url }

# What the tag points at. `git ls-remote` on a tag gives the tag object for an annotated tag and
# the commit for a lightweight one; ^{} is the dereferenced commit and is preferred when present.
$newPin = $Commit
if (-not $newPin) {
    if (-not $Upstream) { throw "could not read [meshcore].url from $ini" }
    $refs = & git ls-remote --tags $Upstream "refs/tags/$Tag" "refs/tags/$Tag^{}" 2>$null
    if ($LASTEXITCODE -ne 0) { throw "could not read the tags of $Upstream" }
    foreach ($line in @($refs)) {
        if ($line -match '^([0-9a-f]{40})\s+refs/tags/\S+\^\{\}$') { $newPin = $Matches[1] }
        elseif (-not $newPin -and $line -match '^([0-9a-f]{40})\s+refs/tags/\S+$') { $newPin = $Matches[1] }
    }
}
if ($newPin -notmatch '^[0-9a-f]{40}$') { throw "$Tag is not a tag of $Upstream" }
if ($newPin -eq $oldPin) { throw "$Tag is the pin this repository is already on ($oldPin) - there is nothing to sync" }

# The next number: the driver reads two digits, so 01..99.
$used = @(Get-ChildItem $syncDir -Filter '[0-9][0-9]-*.md' -File -ErrorAction SilentlyContinue |
    ForEach-Object { [int]$_.Name.Substring(0, 2) })
$n = if ($used.Count -gt 0) { ($used | Measure-Object -Maximum).Maximum + 1 } else { 1 }
if ($n -gt 99) { throw 'ninety-nine sync tasks is enough' }
$nn = '{0:d2}' -f $n

# The name has to match the driver's row regex, which is [a-z0-9-]+ : no dots.
$slug = 'meshcore-' + ($Tag -replace '\.', '-')
$file = Join-Path $syncDir "$nn-$slug.md"
if (Test-Path $file) { throw "$file already exists" }

$prText = if ($Pr -gt 0) { "pull request #$Pr" } else { 'the pull request upstream.yml opened (its number is not recorded here; `gh pr list --label meshcore` finds it)' }
$logText = if ($LogUrl) {
    if ($LogUrl -match '^https?://') { "    $LogUrl" } else { ($LogUrl -split "`n" | ForEach-Object { '    ' + $_ }) -join "`n" }
}
else {
    "    Not recorded when this file was written. `gh run view --log-failed` on the newest red run of the branch."
}

$text = [IO.File]::ReadAllText($template)
$text = $text.Replace('{{NN}}', $nn)
$text = $text.Replace('{{TAG}}', $Tag)
# Invariant, not the machine's locale: the prose of this repository is English.
$text = $text.Replace('{{DATE}}', [datetime]::Now.ToString('d MMMM yyyy', [Globalization.CultureInfo]::InvariantCulture))
$text = $text.Replace('{{OLD_PIN_SHORT}}', $oldPin.Substring(0, 8))
$text = $text.Replace('{{OLD_PIN}}', $oldPin)
$text = $text.Replace('{{NEW_PIN}}', $newPin)
$text = $text.Replace('{{PR}}', $prText)
$text = $text.Replace('{{LOG}}', $logText)
if ($text -match '\{\{([A-Z_]+)\}\}') { throw "$template has a placeholder this script does not fill: {{$($Matches[1])}}" }
[IO.File]::WriteAllText($file, $text, (New-Object Text.UTF8Encoding($false)))

# The row the driver's table needs, inserted under the header of README.md's table.
# ReadAllText, not Get-Content: PowerShell 5.1 reads a UTF-8 file without a BOM as the ANSI
# codepage, and this README is full of em dashes that would come back as mojibake.
$lines = New-Object Collections.Generic.List[string]
foreach ($line in ([IO.File]::ReadAllText($readme) -replace "`r`n", "`n").TrimEnd("`n") -split "`n") { $lines.Add($line) }
$row = "| $nn | [$slug]($nn-$slug.md) | $Tag | CI red on $prText |"
$header = -1
for ($i = 0; $i -lt $lines.Count; $i++) { if ($lines[$i] -match '^\|---\|') { $header = $i; break } }
if ($header -lt 0) { throw "no task table in $readme" }
$lines.Insert($header + 1, $row)
# The "Empty: ..." paragraph under the table stops being true the moment the first row goes in,
# so it goes: the line and the rest of its paragraph, up to the next blank line.
for ($i = 0; $i -lt $lines.Count; $i++) {
    if ($lines[$i] -notmatch '^Empty:') { continue }
    while ($i -lt $lines.Count -and $lines[$i].Trim() -ne '') { $lines.RemoveAt($i) }
    break
}
while ($lines.Count -gt 0 -and $lines[$lines.Count - 1].Trim() -eq '') { $lines.RemoveAt($lines.Count - 1) }
[IO.File]::WriteAllText($readme, (($lines -join "`n") + "`n"), (New-Object Text.UTF8Encoding($false)))

Write-Host "wrote $file"
Write-Host "  MeshCore $oldPin -> $newPin ($Tag)"
Write-Host "  the branch is already on origin: upstream/$Tag"
Write-Host "  run it with: .\tasks\run-all.ps1 -Phase sync -From $nn"
