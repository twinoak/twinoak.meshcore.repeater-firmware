<#
.SYNOPSIS
  Runs several phases one after another with run-all.ps1, and stops at the first one that stops.

.DESCRIPTION
  A pile is more than one phase. This starts run-all.ps1 for each phase in order and goes on to the
  next only when every task of the one before has landed. Everything run-all.ps1 does - the
  preflight, the retries on a usage limit, STOPPED.md - it does here too, once per phase.

  .\tasks\run-pile.ps1                         # phases 0, 1, 2, 3
  .\tasks\run-pile.ps1 -Phases 1,2             # just those
  .\tasks\run-pile.ps1 -Model claude-opus-5    # default model for tasks that name none

  After a stop: read tasks\phase-N\logs\STOPPED.md, fix, and start again - tasks that have
  landed are skipped, so the same command resumes.
#>
[CmdletBinding()]
param(
    [string[]]$Phases = @('0', '1', '2', '3'),
    [string]$Model = ''
)

$runAll = Join-Path $PSScriptRoot 'run-all.ps1'
foreach ($phase in $Phases) {
    $phaseDir = Join-Path $PSScriptRoot "phase-$phase"
    if (-not (Test-Path (Join-Path $phaseDir 'README.md'))) {
        Write-Host "run-pile: no tasks\phase-$phase\README.md - nothing to run there, stopping."
        exit 1
    }
    $runArgs = @{ Phase = $phase }
    if ($Model) { $runArgs['Model'] = $Model }
    & $runAll @runArgs
    $code = $LASTEXITCODE
    if ($code -ne 0 -or (Test-Path (Join-Path $phaseDir 'logs\STOPPED.md'))) {
        Write-Host "run-pile: phase $phase stopped (exit $code). See tasks\phase-$phase\logs\STOPPED.md."
        exit 1
    }
    Write-Host "run-pile: phase $phase is done."
}
Write-Host "run-pile: every phase of the pile is done."
exit 0
