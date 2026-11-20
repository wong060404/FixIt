#requires -version 5.1
<#
.SYNOPSIS
  Puts this FixIt checkout on the user PATH, so `fixit <file>` works from any folder.

.DESCRIPTION
  Run it once.  After that, any new PowerShell or cmd window can do:

    cd C:\some\project
    fixit.cmd main.cpp --agent --llm mock --no-write

  The PATH entry points at the checkout itself (no files are copied), so a plain
  `git pull` keeps the command current.  -Remove undoes it.

.EXAMPLE
  .\tools\windows\install-command.ps1
  .\tools\windows\install-command.ps1 -Remove
#>

param(
  [switch]$Remove
)

$ErrorActionPreference = 'Stop'

function Find-FixItRepo {
  param([string]$Start)
  $dir = (Resolve-Path -LiteralPath $Start).Path
  while ($true) {
    if ((Test-Path (Join-Path $dir 'CMakeLists.txt')) -and
        (Test-Path (Join-Path $dir 'fixit.cmd'))) {
      return $dir
    }
    $parent = Split-Path -Parent $dir
    if (-not $parent -or $parent -eq $dir) { break }
    $dir = $parent
  }
  throw "FixIt checkout not found above $Start"
}

$repo = Find-FixItRepo $PSScriptRoot
. (Join-Path $PSScriptRoot 'fixit-env.ps1')
if (-not (Test-FixitAsciiPath $repo)) {
  Write-Host 'warning: this checkout path contains non-ASCII characters:'
  Write-Host "  $repo"
  Write-Host 'the command will be installed, but gcc cannot build from such a path.'
}

# Current user PATH, kept in order and never rewritten wholesale.
$current = [Environment]::GetEnvironmentVariable('Path', 'User')
if ($null -eq $current) { $current = '' }
$parts = @($current -split ';' | ForEach-Object { $_.Trim() } | Where-Object { $_ })
$present = @($parts | Where-Object { $_ -ieq $repo }).Count -gt 0

# Windows already has one user-writable folder on PATH (the app-alias folder).
# It is the fallback when the registry cannot be written, which is what happens
# in a locked-down or sandboxed session.
$shimDir = Join-Path $env:LOCALAPPDATA 'Microsoft\WindowsApps'
$shim = Join-Path $shimDir 'fixit.cmd'

if ($Remove) {
  if ($present) {
    $parts = @($parts | Where-Object { $_ -ine $repo })
    try {
      [Environment]::SetEnvironmentVariable('Path', ($parts -join ';'), 'User')
      Write-Host "removed from the user PATH: $repo"
    } catch {
      Write-Host "could not update the user PATH: $($_.Exception.Message)"
    }
    $env:PATH = ($parts -join ';')
  } else {
    Write-Host "not on the user PATH: $repo"
  }
  if (Test-Path $shim) {
    try { Remove-Item $shim -Force; Write-Host "removed shim: $shim" }
    catch { Write-Host "could not remove the shim: $($_.Exception.Message)" }
  }
  exit 0
}

if (-not $present) { $parts += $repo }
$updated = ($parts -join ';')
$persisted = $false
try {
  [Environment]::SetEnvironmentVariable('Path', $updated, 'User')
  $persisted = $true
  $env:PATH = $updated
  Write-Host "added to the user PATH: $repo"
} catch {
  Write-Host "could not update the user PATH: $($_.Exception.Message)"
}

if (-not $persisted) {
  if (Test-Path $shimDir) {
    $content = '@echo off' + "`r`n" + '"' + (Join-Path $repo 'fixit.cmd') + '" %*' + "`r`n"
    try {
      Set-Content -Path $shim -Value $content -Encoding ascii
      Write-Host "installed a shim instead: $shim"
      $env:PATH = $shimDir + ';' + $env:PATH
    } catch {
      Write-Host "could not write the shim: $($_.Exception.Message)"
      Write-Host 'add this folder to PATH manually:'
      Write-Host "  $repo"
    }
  } else {
    Write-Host 'add this folder to PATH manually:'
    Write-Host "  $repo"
  }
}

$found = Get-Command 'fixit.cmd' -ErrorAction SilentlyContinue
if ($found) {
  Write-Host "check: fixit.cmd -> $($found.Source)"
} else {
  Write-Host 'check: fixit.cmd is not resolvable yet; open a new PowerShell window.'
}
Write-Host ''
Write-Host 'usage, from any folder:'
Write-Host '  fixit.cmd main.cpp --agent --llm mock --verbose --no-write'
Write-Host '  fixit.cmd Downloads\main.cpp          # bare names are resolved for you'