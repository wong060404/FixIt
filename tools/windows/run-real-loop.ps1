# Runs FixIt's full agent loop against a real OpenAI-compatible endpoint.
#
#   .\tools\windows\run-real-loop.ps1 -BaseUrl https://your-endpoint/v1 -Model your-model
#
# What it does:
#   1. starts tools\windows\llm-relay.py on 127.0.0.1 over plain HTTP (this build
#      of FixIt has no TLS) forwarding to your https endpoint, with the key read
#      from <repository>/.secrets/fixit_key -- the key never enters fixit's argv,
#      its environment or its trace;
#   2. runs the loop: compile -> locate -> model patch -> fuzzy apply -> re-verify,
#      with --verbose, --trace and --metrics;
#   3. prints the transcript, the per-round relay log, the metrics and a
#      before/after diff, then stops the relay.
#
# Without -File it creates a small file with one deliberate bug, so the whole
# thing can be tried with nothing prepared.  That file is repaired in place.

param(
  [Parameter(Mandatory = $true)][string]$BaseUrl,
  [Parameter(Mandatory = $true)][string]$Model,
  [string]$File,
  [int]$Port = 8791,
  [int]$Iterations = 4,
  [switch]$NoWrite
)

$ErrorActionPreference = 'Continue'

function Find-FixItRepo {
  param([string]$Start)
  $dir = (Resolve-Path -LiteralPath $Start).Path
  while ($true) {
    if ((Test-Path (Join-Path $dir 'CMakeLists.txt')) -and
        (Test-Path (Join-Path $dir 'src\compiler.cpp'))) {
      return $dir
    }
    $parent = Split-Path -Parent $dir
    if (-not $parent -or $parent -eq $dir) { break }
    $dir = $parent
  }
  throw "FixIt repository not found above $Start"
}

$repo = Find-FixItRepo $PSScriptRoot
$fixit = Join-Path $repo 'build-win\bin\fixit.exe'
$relayScript = Join-Path $repo 'tools\windows\llm-relay.py'
$cache = Join-Path $repo '.cache'
New-Item -ItemType Directory -Force -Path $cache | Out-Null

$tc = if ($env:FIXIT_TOOLCHAIN) { $env:FIXIT_TOOLCHAIN }
      else { Join-Path (Split-Path -Parent $repo) 'toolchain' }
if (-not (Test-Path (Join-Path $tc 'mingw\mingw64\bin\g++.exe'))) {
  $tc = Join-Path $env:TEMP 'fixit-toolchain'
}
$keyFile = if ($env:RELAY_KEY_FILE) { $env:RELAY_KEY_FILE }
           else { Join-Path $repo '.secrets\fixit_key' }

if (-not (Test-Path $fixit)) { throw "not built yet: $fixit  (run tools\windows\build_fixit.ps1)" }
if (-not (Test-Path $keyFile)) {
  throw "no API key: put it in $keyFile or set RELAY_KEY_FILE"
}

if (-not $File) {
  $demoDir = Join-Path $env:TEMP 'fixit-demo'
  New-Item -ItemType Directory -Force -Path $demoDir | Out-Null
  $File = Join-Path $demoDir 'demo_bug.cpp'
  @'
#include <iostream>
#include <string>

int main() {
    std::string greeting = "Hello, FixIt!";
    std::cout << Greeting << std::endl;
    return 0;
}
'@ | Set-Content -Path $File -Encoding ascii
  Write-Host "no -File given; created a file with one deliberate bug:"
  Write-Host "  $File"
}
if (-not (Test-Path $File)) { throw "no such file: $File" }

$env:PATH = "$tc\mingw\mingw64\bin;$env:PATH"   # fixit's default compiler is g++
$env:NO_COLOR = '1'

$upstreamPath = ([System.Uri]$BaseUrl).AbsolutePath.TrimEnd('/')
$localBase = "http://127.0.0.1:$Port$upstreamPath"

$transcript = Join-Path $cache 'llm-loop-transcript.txt'
$trace = Join-Path $cache 'llm-loop-trace.json'
$metrics = Join-Path $cache 'llm-loop-metrics.json'
$relayLog = Join-Path $cache 'llm-relay.log.jsonl'
$relayOut = Join-Path $cache 'llm-relay.out.txt'
$relayErr = Join-Path $cache 'llm-relay.err.txt'
$snapshot = "$File.before"

Remove-Item $transcript, $trace, $metrics, $relayLog, $relayOut, $relayErr -ErrorAction SilentlyContinue
Copy-Item $File $snapshot -Force

$env:RELAY_TARGET = $BaseUrl
$env:RELAY_PORT = "$Port"
$relay = Start-Process -FilePath (Get-Command python).Source `
  -ArgumentList "`"$relayScript`"" -WorkingDirectory $repo -PassThru -WindowStyle Hidden `
  -RedirectStandardOutput $relayOut -RedirectStandardError $relayErr

$ready = $false
try {
  # Readiness is not "something answers on the port": a relay someone else left
  # running for another endpoint would happily take our requests and forward them
  # to *their* endpoint with *their* key.  Ask the responder who it is.
  $occupant = $null
  foreach ($attempt in 1..40) {
    Start-Sleep -Milliseconds 500
    $info = $null
    try {
      $info = (Invoke-WebRequest -Uri "http://127.0.0.1:$Port/" -UseBasicParsing -TimeoutSec 3).Content |
        ConvertFrom-Json
    } catch { $info = $null }
    if ($null -ne $info -and $info.relay -eq 'ok') {
      if ($info.target -eq $BaseUrl) { $ready = $true } else { $occupant = $info.target }
      break
    }
    if ($relay.HasExited) { break }
  }

  if (-not $ready) {
    if ($occupant) {
      Write-Host "port $Port is already serving a relay for a different endpoint:"
      Write-Host "  in use    : $occupant"
      Write-Host "  you asked : $BaseUrl"
      Write-Host 'start a second relay on another port with -Port <number>, or stop the running one first.'
    } else {
      Write-Host '--- relay output ---'
      Get-Content $relayOut, $relayErr -ErrorAction SilentlyContinue
    }
    throw 'relay did not start'
  }
  Get-Content $relayOut -ErrorAction SilentlyContinue | Select-Object -First 4

  $fixitArgs = @(
    $File, '--agent', '--llm', 'openai',
    '--base-url', $localBase, '--model', $Model, '--api-key', 'relay',
    '--iterations', "$Iterations", '--verbose',
    '--trace', $trace, '--metrics', $metrics
  )
  if ($NoWrite) { $fixitArgs += '--no-write' }

  Write-Host ''
  Write-Host "=== fixit ==="
  & $fixit @fixitArgs 2>&1 | Tee-Object -FilePath $transcript
  $fixitExit = $LASTEXITCODE

  Write-Host ''
  Write-Host "=== exit code: $fixitExit ==="
  Write-Host '--- metrics ---'
  Get-Content $metrics -ErrorAction SilentlyContinue
  Write-Host '--- relay per-round log (no key material) ---'
  Get-Content $relayLog -ErrorAction SilentlyContinue
  if (-not $NoWrite) {
    Write-Host '--- diff (before -> after) ---'
    & git diff --no-index --unified=3 $snapshot $File
  }
} finally {
  # Ask the relay to stop itself: `python` here is a Store app alias, so the PID
  # we hold is a launcher, and killing it alone would orphan the real relay
  # (which then keeps this script's redirected stdout open).  Only ever stop a
  # relay this run successfully bound; taskkill is a fallback for a hung one.
  if ($ready) {
    try {
      Invoke-WebRequest -Uri "http://127.0.0.1:$Port/shutdown" -Method POST -UseBasicParsing -TimeoutSec 5 |
        Out-Null
    } catch { }
    Start-Sleep -Milliseconds 500
    if ($relay -and -not $relay.HasExited) { & taskkill /PID $relay.Id /T /F 2>&1 | Out-Null }
  }
  Remove-Item Env:RELAY_TARGET, Env:RELAY_PORT -ErrorAction SilentlyContinue
  Write-Host ''
  if ($ready) { Write-Host 'relay stopped' }
}
