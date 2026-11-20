#requires -version 5.1
<#
.SYNOPSIS
  One-time setup after cloning: find (or download) a compiler, build FixIt, and register
  the `fixit` command so it works from any folder on this machine.

.DESCRIPTION
  Run it once per machine.  It does not install anything system-wide and it never copies
  files into your projects: the command is registered as a user PATH entry (or a small
  shim in %LOCALAPPDATA%\Microsoft\WindowsApps when the registry is not writable), both
  pointing back at this checkout.  Re-running it is harmless.

.EXAMPLE
  .\setup.cmd
  powershell -ExecutionPolicy Bypass -File .\setup.ps1 -SkipInstall
#>

param(
  [switch]$SkipInstall,
  [switch]$FetchToolchain
)

$ErrorActionPreference = 'Stop'
$repo = $PSScriptRoot

. (Join-Path $repo 'tools\windows\fixit-env.ps1')

Write-Host '=== FixIt setup ==='
Write-Host "checkout: $repo"


# 1. compiler ---------------------------------------------------------------
$compiler = Find-FixitCompilerBin -Repo $repo
if (-not $compiler -and $FetchToolchain) {
  Write-Host 'no compiler found; downloading the portable MinGW GCC (~274 MB) ...'
  $python = Get-Command 'python' -ErrorAction SilentlyContinue
  if (-not $python) { $python = Get-Command 'py' -ErrorAction SilentlyContinue }
  if (-not $python) {
    Write-Host 'python is required to download the toolchain; install Python 3, or install MSYS2.'
    exit 2
  }
  & $python.Source (Join-Path $repo 'tools\windows\fetch_mingw.py')
  if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
  $compiler = Find-FixitCompilerBin -Repo $repo
}
if (-not $compiler) {
  Write-Host 'no C++ compiler found.  Options:'
  Write-Host '  1) install MSYS2 (it provides g++) and run this script again, or'
  Write-Host '  2) run:  .\setup.cmd -FetchToolchain   (downloads a portable GCC, ~274 MB)'
  exit 2
}
Write-Host "compiler: $compiler"

# 2. build ------------------------------------------------------------------
$exe = Join-Path $repo 'build-win\bin\fixit.exe'
$stale = $true
if (Test-Path $exe) {
  $stale = $false
  $exeTime = (Get-Item $exe).LastWriteTimeUtc
  $sources = @()
  $sources += @(Get-ChildItem (Join-Path $repo 'src') -Filter '*.cpp' -ErrorAction SilentlyContinue)
  $sources += @(Get-ChildItem (Join-Path $repo 'include\fixit') -Filter '*.h' -ErrorAction SilentlyContinue)
  $sources += @(Get-Item (Join-Path $repo 'tools\fixit-cli\main.cpp') -ErrorAction SilentlyContinue)
  foreach ($source in $sources) {
    if ($source.LastWriteTimeUtc -gt $exeTime) { $stale = $true; break }
  }
}
Write-Host ''
if ($stale) {
  Write-Host 'building (about a minute the first time) ...'
  & (Join-Path $repo 'tools\windows\build_fixit.ps1') -CompilerBin $compiler
} else {
  Write-Host 'build is up to date.'
}
if (-not (Test-Path $exe)) {
  Write-Host 'build finished but the binary is missing.'
  exit 2
}
Write-Host "built: $exe"

# 3. register the command ---------------------------------------------------
if (-not $SkipInstall) {
  Write-Host ''
  Write-Host 'registering the fixit command ...'
  try {
    & (Join-Path $repo 'tools\windows\install-command.ps1')
  } catch {
    Write-Host "could not register the command: $($_.Exception.Message)"
    Write-Host 'you can still run it by its full path; to register manually, add this folder to PATH:'
    Write-Host "  $repo"
  }
}

# 4. verify -----------------------------------------------------------------
Write-Host ''
$found = Get-Command 'fixit.cmd' -ErrorAction SilentlyContinue
if ($found) {
  Write-Host "ready: fixit.cmd -> $($found.Source)"
  Write-Host ''
  Write-Host 'from any folder now:'
  Write-Host '  fixit.cmd main.cpp --agent --llm mock --verbose --no-write'
} else {
  Write-Host 'the command is not on PATH in this window yet.'
  Write-Host 'open a new PowerShell window, or use the full path:'
  Write-Host "  $repo\fixit.cmd main.cpp --agent --llm mock --no-write"
}

Write-Host ''
Write-Host 'setup finished.'