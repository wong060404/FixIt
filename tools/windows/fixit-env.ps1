# Shared helpers for the Windows scripts.  Dot-source it:
#
#   . "$PSScriptRoot\fixit-env.ps1"
#
# Finds a C++ toolchain without editing the environment: every candidate is
# used through its absolute path.  A candidate has to provide gcc.exe,
# g++.exe and ar.exe, because both the build and the repair loop use all
# three.

# Join-Path throws when the base names a drive that does not exist (for example a
# TEMP pointing at an unmounted drive).  Candidate discovery must degrade, not die.
function Join-FixitPath {
  param([string]$Base, [string]$Child)
  if (-not $Base) { return '' }
  try { return (Join-Path $Base $Child) } catch { return '' }
}

function Test-FixitCompilerBin {
  param([string]$Dir)
  if (-not $Dir) { return $false }
  try {
    return (Test-Path (Join-Path $Dir "gcc.exe")) -and
           (Test-Path (Join-Path $Dir "g++.exe")) -and
           (Test-Path (Join-Path $Dir "ar.exe"))
  } catch { return $false }
}


function Find-FixitCompilerBin {
  param(
    [string]$Repo,
    [string]$Preferred = ""
  )
  $candidates = @()
  if ($Preferred) { $candidates += $Preferred }
  # winlibs changed its archive layout: older releases unpack to
  # <root>\mingw\mingw64\bin, r3 and later to <root>\mingw64\bin.  Accept both.
  $roots = @()
  if ($env:FIXIT_TOOLCHAIN) { $roots += $env:FIXIT_TOOLCHAIN }
  if ($Repo) { $roots += (Join-FixitPath (Split-Path -Parent $Repo) "toolchain") }
  $roots += (Join-FixitPath $env:TEMP "fixit-toolchain")
  foreach ($root in $roots) {
    foreach ($relative in "mingw64\bin", "mingw\mingw64\bin") {
      $candidates += (Join-FixitPath $root $relative)
    }
  }
  $onPath = Get-Command "g++.exe" -ErrorAction SilentlyContinue
  if (-not $onPath) { $onPath = Get-Command "g++" -ErrorAction SilentlyContinue }
  if ($onPath) { $candidates += (Split-Path -Parent $onPath.Source) }
  if ($env:SystemDrive) {
    $msys = Join-Path $env:SystemDrive "msys64"
    foreach ($variant in "ucrt64", "mingw64", "mingw32") {
      $candidates += (Join-Path $msys "$variant\bin")
    }
  }
  foreach ($candidate in $candidates) {
    if (Test-FixitCompilerBin $candidate) {
      return (Resolve-Path -LiteralPath $candidate).Path
    }
  }
  return ""
}