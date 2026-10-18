<#
.SYNOPSIS
  Verifies the repository the way CI does - and CI runs this script and nothing else.

.DESCRIPTION
  One command is the definition of "verified" here (spec/SYSTEM.md S-10, tasks/README.md):

    1. spec     python spec/tools/specsync.py verify spec        - the copy of the specification
                                                                   is byte for byte its manifest's
    2. link     python spec/tools/linkref.py check <fixture>     - the link fixture is well formed
    3. build    pio run -e <env> for every board environment of platformio.ini (every [env:*]
                but native - read from the file, so there is no second list to forget); the
                images are copied to out/build/<env>/
    4. native   pio test -e native - with this machine's own g++ or clang++ when there is one,
                otherwise in the container tasks/RUNBOOK.md section 2 names (Docker) - and then
                bash tools/linkloop/check.sh, in the same place and with the same compiler: the
                box link's own session, clock keeper, settings table and first-boot profile built
                as a host program and driven by tools/linkcli.py through a pipe. One step, two
                rows of the summary, because they are two things that can fail; the link loop is
                in this step because it needs exactly what this step already has
    5. release  python tools/release/selfcheck.py --require-descriptor - the release tooling's
                own tests, then a dry run of the real release over out/build/<env>, what step 3
                left: the images, each one's descriptor held against its manifest entry, a
                validated manifest, its signature by the specification's public test key, and
                that signature verified. So the release path is exercised on every commit without
                GitHub and without the release key. It needs the `cryptography` of
                tools/release/requirements.txt: this machine's Python when it has it, otherwise
                the same container as step 4, where pip installs it into the container and not
                into this machine.

  Every step's output goes to out/verify/<step>.log, a one-line summary per step is printed at
  the end, and the exit code is non-zero when anything failed. Runs on Windows PowerShell 5.1 and
  on pwsh under Linux (CI).

  pio need not be on PATH: like the driver's preflight (tasks/run-all.ps1), the script looks in
  ~/.platformio/penv for the copy the VS Code extension installs. Python is the one PlatformIO
  itself runs on (`pio system info`), which is always there.

    .\tools\verify.ps1               everything
    .\tools\verify.ps1 -Only build   one step (spec, link, build, native, release)

  -Only native runs the native tests and the link loop both: they are one step.
#>
[CmdletBinding()]
param(
    [ValidateSet('', 'spec', 'link', 'build', 'native', 'release')]
    [string]$Only = ''
)

$ErrorActionPreference = 'Continue'   # native commands write to stderr; exit codes are checked by hand
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$outDir = Join-Path $root 'out'
$logDir = Join-Path $outDir 'verify'
New-Item -ItemType Directory -Force -Path $logDir | Out-Null
try { [Console]::OutputEncoding = [Text.Encoding]::UTF8 } catch { }
$env:PLATFORMIO_DISABLE_PROGRESSBAR = 'true'
$env:PYTHONUNBUFFERED = '1'

$results = @()   # one row per step: Name, Ok, Log, Note

function Write-Line([string]$m) { Write-Host $m }

function Invoke-Logged {
    # Runs a native command, streams its output to the console and to a log, returns the exit code.
    param([string]$Log, [string]$Exe, [string[]]$Arguments)
    "> $Exe $($Arguments -join ' ')" | Out-File -FilePath $Log -Encoding utf8
    & $Exe @Arguments 2>&1 | ForEach-Object {
        $line = if ($_ -is [System.Management.Automation.ErrorRecord]) { $_.Exception.Message } else { "$_" }
        Write-Host $line
        $line
    } | Out-File -FilePath $Log -Encoding utf8 -Append
    $code = $LASTEXITCODE
    "exit code: $code" | Out-File -FilePath $Log -Encoding utf8 -Append
    return $code
}

function Add-Result([string]$Name, [bool]$Ok, [string]$Log, [string]$Note = '') {
    $script:results += [pscustomobject]@{ Name = $Name; Ok = $Ok; Log = $Log; Note = $Note }
}

function Find-Pio {
    $cmd = Get-Command pio -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    foreach ($dir in @((Join-Path $HOME '.platformio/penv/Scripts'), (Join-Path $HOME '.platformio/penv/bin'))) {
        foreach ($name in @('pio.exe', 'pio')) {
            $p = Join-Path $dir $name
            if (Test-Path $p) { return $p }
        }
    }
    return $null
}

function Find-Python([string]$Pio) {
    # The Python PlatformIO runs on: pio system info prints "Python Executable  <path>".
    if ($Pio) {
        $info = & $Pio --no-ansi system info 2>$null
        foreach ($line in $info) {
            if ("$line" -match '^\s*Python Executable\s+(.+?)\s*$') {
                $p = $Matches[1]
                if (Test-Path $p) { return $p }
            }
        }
    }
    foreach ($name in @('python3', 'python')) {
        $cmd = Get-Command $name -ErrorAction SilentlyContinue
        if ($cmd) { return $cmd.Source }
    }
    return $null
}

function Get-BoardEnvironments {
    # Every [env:<name>] of platformio.ini but native.
    $envs = @()
    foreach ($line in Get-Content (Join-Path $root 'platformio.ini')) {
        if ($line -match '^\[env:([^\]]+)\]') {
            $name = $Matches[1].Trim()
            if ($name -ne 'native') { $envs += $name }
        }
    }
    return $envs
}

function Get-MemoryLines([string]$Log) {
    $lines = @()
    foreach ($line in Get-Content $Log) {
        if ($line -match '^(RAM|Flash):\s+\[.*?\]\s+([0-9.]+%\s+\(used \d+ bytes from \d+ bytes\))') {
            $lines += ("{0} {1}" -f $Matches[1], $Matches[2])
        }
    }
    return ($lines -join '; ')
}

Set-Location $root
$pio = Find-Pio
$python = Find-Python $pio
Write-Line "verify: $root"
Write-Line ("pio:    " + $(if ($pio) { $pio } else { 'NOT FOUND' }))
Write-Line ("python: " + $(if ($python) { $python } else { 'NOT FOUND' }))
$pioVersion = ''
if ($pio) {
    $pioVersion = ((& $pio --version 2>$null) -join ' ') -replace '^.*version\s+', ''
    Write-Line "pio version: $pioVersion"
}

function Step-Wanted([string]$name) { return (-not $Only) -or ($Only -eq $name) }

# 1. spec ------------------------------------------------------------------------------------
if (Step-Wanted 'spec') {
    $log = Join-Path $logDir 'spec.log'
    Write-Line "`n== spec: the copy of the specification matches spec/MANIFEST.txt"
    if (-not $python) { Add-Result 'spec' $false $log 'no python'; }
    else {
        $code = Invoke-Logged $log $python @('spec/tools/specsync.py', 'verify', 'spec')
        Add-Result 'spec' ($code -eq 0) $log
    }
}

# 2. link ------------------------------------------------------------------------------------
if (Step-Wanted 'link') {
    $log = Join-Path $logDir 'link.log'
    Write-Line "`n== link: the link fixture checks"
    if (-not $python) { Add-Result 'link' $false $log 'no python' }
    else {
        $code = Invoke-Logged $log $python @('spec/tools/linkref.py', 'check', 'spec/fixtures/link/link-v1.json')
        Add-Result 'link' ($code -eq 0) $log
    }
}

# 3. build -----------------------------------------------------------------------------------
if (Step-Wanted 'build') {
    $envs = Get-BoardEnvironments
    if ($envs.Count -eq 0) { Add-Result 'build' $false (Join-Path $logDir 'build.log') 'no board environment in platformio.ini' }
    foreach ($e in $envs) {
        $log = Join-Path $logDir "build-$e.log"
        Write-Line "`n== build: pio run -e $e"
        if (-not $pio) { Add-Result "build $e" $false $log 'no pio'; continue }
        $code = Invoke-Logged $log $pio @('--no-ansi', 'run', '-e', $e)
        $ok = ($code -eq 0)
        $note = ''
        if ($ok) {
            # The images, kept where a reviewer and the release step find them (tasks/README.md:
            # leave the evidence). hex: the Walter over SWD. zip: adafruit-nrfutil DFU over USB.
            # uf2: the bootloader's drive. elf: the debugger. buildinfo.json and ldscript.ld:
            # what the build knew about itself, which the release tooling reads instead of being
            # told (tools/pio/buildinfo.py). map: the linker map, which object every symbol came
            # from (tools/pio/linkmap.py).
            #
            # The release step reads this copy and not .pio/build/<env>, because the native step
            # runs PlatformIO inside a container: a different Python and a different core see a
            # different project checksum, and PlatformIO then cleans .pio/build out from under
            # whatever comes after it. out/build/<env> is the build output that survives the run.
            $dest = Join-Path (Join-Path $outDir 'build') $e
            New-Item -ItemType Directory -Force -Path $dest | Out-Null
            foreach ($name in @('firmware.hex', 'firmware.bin', 'firmware.zip', 'firmware.uf2',
                                'firmware.elf', 'firmware.map', 'buildinfo.json', 'ldscript.ld')) {
                $src = Join-Path $root ".pio/build/$e/$name"
                if (Test-Path $src) { Copy-Item $src (Join-Path $dest $name) -Force }
                else { $ok = $false; $note = "$name missing" }
            }
            if ($ok) { $note = Get-MemoryLines $log }
        }
        Add-Result "build $e" $ok $log $note
    }
}

# 4. native ----------------------------------------------------------------------------------
if (Step-Wanted 'native') {
    $log = Join-Path $logDir 'native.log'
    Write-Line "`n== native: pio test -e native"
    $cxx = @('g++', 'clang++') | Where-Object { Get-Command $_ -ErrorAction SilentlyContinue } | Select-Object -First 1
    if ($cxx -and $pio) {
        $code = Invoke-Logged $log $pio @('--no-ansi', 'test', '-e', 'native')
        Add-Result 'native' ($code -eq 0) $log "with this machine's $cxx"
    }
    elseif (Get-Command docker -ErrorAction SilentlyContinue) {
        # No host compiler (a Windows box): the container of tasks/RUNBOOK.md section 2, with the
        # same PlatformIO version as the one here so the two runs are the same run.
        $ver = if ($pioVersion) { "==$pioVersion" } else { '' }
        $mount = ($root -replace '\\', '/') + ':/w'
        $inner = "apt-get update -qq && apt-get install -y -qq g++ git > /dev/null && pip install -q platformio$ver && pio --no-ansi test -e native"
        $code = Invoke-Logged $log 'docker' @('run', '--rm', '-v', $mount, '-w', '/w',
            '-e', 'PLATFORMIO_DISABLE_PROGRESSBAR=true', '-e', 'DEBIAN_FRONTEND=noninteractive',
            '-e', 'PIP_ROOT_USER_ACTION=ignore', '-e', 'PIP_DISABLE_PIP_VERSION_CHECK=1',
            'python:3.11-slim', 'bash', '-c', $inner)
        Add-Result 'native' ($code -eq 0) $log 'in a container (no g++ on this machine)'
    }
    else {
        "no g++, no clang++ and no docker: pio test -e native cannot run here" | Out-File -FilePath $log -Encoding utf8
        Add-Result 'native' $false $log 'no compiler and no docker'
    }

    # tools/linkloop/check.sh, in the same step: the same host compiler, the same container image,
    # its own log and its own row. Before this it was in no gate at all, and p3-01's move of src/
    # left its includes stale with nothing to notice (tasks/phase-3/reports/03.md; the phase-3
    # review made closing that this task's). It needs g++ and python3 - what the native tests
    # need - and no PlatformIO, so the container run installs nothing here: check.sh's own first
    # lines apt-get g++ when there is none, and python:3.11-slim is python3.
    $linkLog = Join-Path $logDir 'linkloop.log'
    Write-Line "`n== native: bash tools/linkloop/check.sh"
    $bash = Get-Command bash -ErrorAction SilentlyContinue
    if ($cxx -and $bash) {
        $code = Invoke-Logged $linkLog $bash.Source @('tools/linkloop/check.sh')
        Add-Result 'linkloop' ($code -eq 0) $linkLog "with this machine's $cxx"
    }
    elseif (Get-Command docker -ErrorAction SilentlyContinue) {
        $mount = ($root -replace '\\', '/') + ':/w'
        $code = Invoke-Logged $linkLog 'docker' @('run', '--rm', '-v', $mount, '-w', '/w',
            '-e', 'DEBIAN_FRONTEND=noninteractive',
            'python:3.11-slim', 'bash', 'tools/linkloop/check.sh')
        Add-Result 'linkloop' ($code -eq 0) $linkLog 'in a container (no g++ on this machine)'
    }
    else {
        "no g++, no clang++ and no docker: tools/linkloop/check.sh cannot run here" |
            Out-File -FilePath $linkLog -Encoding utf8
        Add-Result 'linkloop' $false $linkLog 'no compiler and no docker'
    }
}

# 5. release ---------------------------------------------------------------------------------
if (Step-Wanted 'release') {
    $log = Join-Path $logDir 'release.log'
    Write-Line "`n== release: the release tooling's tests and a dry run over the build"
    # The commit the manifest records. A dry run over a working tree with uncommitted changes
    # names the commit the tree came from, which is what a rehearsal can know.
    $commit = (& git -C $root rev-parse HEAD 2>$null)
    if ($LASTEXITCODE -ne 0 -or -not ($commit -match '^[0-9a-f]{40}$')) { $commit = '0' * 40 }
    # out/build/<env>, what step 3 copied there: see the note in that step for why not .pio/.
    # --require-descriptor: every image this repository builds carries one (phase 3's task 01),
    # the release workflow passes the same flag, and a dry run that did not would be rehearsing
    # a weaker release than the real one.
    $selfcheck = @('tools/release/selfcheck.py', '--commit', "$commit", '--require-descriptor')
    foreach ($e in (Get-BoardEnvironments)) { $selfcheck += @('--build-dir', "out/build/$e") }

    $hasCrypto = $false
    if ($python) {
        & $python -c 'import cryptography' 2>$null | Out-Null
        $hasCrypto = ($LASTEXITCODE -eq 0)
    }
    if (-not $python) { Add-Result 'release' $false $log 'no python' }
    elseif ($hasCrypto) {
        $code = Invoke-Logged $log $python $selfcheck
        Add-Result 'release' ($code -eq 0) $log "with this machine's python"
    }
    elseif (Get-Command docker -ErrorAction SilentlyContinue) {
        # No cryptography here, and nothing is installed into this machine's own Python: the
        # container of step 4, where pip may do as it likes.
        $mount = ($root -replace '\\', '/') + ':/w'
        $inner = "pip install -q -r tools/release/requirements.txt && python " + ($selfcheck -join ' ')
        $code = Invoke-Logged $log 'docker' @('run', '--rm', '-v', $mount, '-w', '/w',
            '-e', 'PIP_ROOT_USER_ACTION=ignore', '-e', 'PIP_DISABLE_PIP_VERSION_CHECK=1',
            'python:3.11-slim', 'bash', '-c', $inner)
        Add-Result 'release' ($code -eq 0) $log 'in a container (no cryptography on this machine)'
    }
    else {
        "no cryptography in this machine's python and no docker: the release step cannot run here" |
            Out-File -FilePath $log -Encoding utf8
        Add-Result 'release' $false $log 'no cryptography and no docker'
    }
}

# Summary ------------------------------------------------------------------------------------
Write-Line "`n== summary"
$failed = 0
foreach ($r in $results) {
    $state = if ($r.Ok) { 'ok  ' } else { 'FAIL'; }
    if (-not $r.Ok) { $failed++ }
    $rel = $r.Log.Substring($root.Length).TrimStart('\', '/')
    $note = if ($r.Note) { " - $($r.Note)" } else { '' }
    Write-Line ("{0} {1,-32} {2}{3}" -f $state, $r.Name, $rel, $note)
}
if ($results.Count -eq 0) { Write-Line "nothing ran"; exit 2 }
if ($failed -gt 0) { Write-Line "$failed step(s) failed"; exit 1 }
Write-Line "all green"
exit 0
