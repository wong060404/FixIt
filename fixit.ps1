#requires -version 5.1
<#
.SYNOPSIS
  Builds FixIt on first use, then runs it.  No environment variables, no PATH
  edits, no cmake: a fresh clone is ready after this one command.

.DESCRIPTION
  Everything is resolved from this script's own location, so the checkout can
  live in any folder on any drive.  The repair target can be in any other
  project folder: pass an absolute path, a relative one, or a path with spaces.

  Each argument is forwarded to fixit.exe unchanged.  Two launcher-only flags
  are consumed here instead:
    --fixit-rebuild           rebuild even when build-win is up to date
    --fixit-fetch-toolchain   download the portable MinGW GCC first

.EXAMPLE
  .\fixit.ps1 "C:\Users\me\my-project\main.cpp" --agent --llm mock --no-write
  .\fixit.ps1 examples\buggy\e1_missing_include.cpp --outline
#>

$ErrorActionPreference = 'Stop'

$repo = $PSScriptRoot
$exe = Join-Path $repo 'build-win\bin\fixit.exe'

# No param block on purpose: an advanced function would claim --verbose as the
# common parameter -Verbose, and fixit's own --verbose would silently vanish.
$rebuild = $false
$fetch = $false
$fileArgs = New-Object System.Collections.Generic.List[string]
foreach ($argument in @($args)) {
  switch ($argument) {
    '--fixit-rebuild' { $rebuild = $true }
    '--fixit-fetch-toolchain' { $fetch = $true }
    default { $fileArgs.Add($argument) }
  }
}

. (Join-Path $repo 'tools\windows\fixit-env.ps1')

$compilerBin = Find-FixitCompilerBin -Repo $repo

if ($fetch) {
  $python = Get-Command 'python' -ErrorAction SilentlyContinue
  if (-not $python) { $python = Get-Command 'py' -ErrorAction SilentlyContinue }
  if (-not $python) {
    Write-Host 'python is required to download the portable toolchain; install it first.'
    exit 2
  }
  & $python.Source (Join-Path $repo 'tools\windows\fetch_mingw.py')
  if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
  $compilerBin = Find-FixitCompilerBin -Repo $repo
}

# Rebuild when the binary is missing, or older than any source it was built from.
$needBuild = $rebuild -or (-not (Test-Path $exe))
if (-not $needBuild) {
  $exeTime = (Get-Item $exe).LastWriteTimeUtc
  $sources = @()
  $sources += @(Get-ChildItem (Join-Path $repo 'src') -Filter '*.cpp' -ErrorAction SilentlyContinue)
  $sources += @(Get-ChildItem (Join-Path $repo 'include\fixit') -Filter '*.h' -ErrorAction SilentlyContinue)
  $sources += @(Get-Item (Join-Path $repo 'tools\fixit-cli\main.cpp') -ErrorAction SilentlyContinue)
  foreach ($source in $sources) {
    if ($source.LastWriteTimeUtc -gt $exeTime) { $needBuild = $true; break }
  }
}

if ($needBuild) {
  Write-Host 'building fixit (first run, or sources changed) ...'
  try {
    & (Join-Path $repo 'tools\windows\build_fixit.ps1') -CompilerBin $compilerBin
  } catch {
    Write-Host ''
    Write-Host 'build failed:' $_.Exception.Message
    if (-not $compilerBin) {
      Write-Host 'no C++ compiler was found.  Download a portable one with:'
      Write-Host '  .\fixit.ps1 --fixit-fetch-toolchain --help'
      Write-Host '(or install MSYS2 / put g++ on PATH), then run the same command again.'
    }
    exit 2
  }
  if (-not (Test-Path $exe)) {
    Write-Host 'build finished but the binary is missing; nothing was run.'
    exit 2
  }
}

# Make the discovered compiler visible to fixit through PATH only for this
# process tree; the user's environment is never modified.
if ($compilerBin -and ($env:PATH -notlike "*$compilerBin*")) {
  $env:PATH = "$compilerBin;$env:PATH"
}

# The console has to speak UTF-8 or fixit's box drawing arrives as mojibake.
# The previous code page is restored afterwards.
$savedCodePage = $null
try {
  $current = ("$(& chcp.com 2>$null)" -join ' ')
  if ($current -match '(\d+)') { $savedCodePage = $Matches[1] }
} catch { }
if ($savedCodePage -and $savedCodePage -ne '65001') {
  & chcp.com 65001 > $null
}


$exitCode = 0
try {
  if ($fileArgs.Count -eq 0) { $fileArgs.Add('--help') }
  & $exe @($fileArgs.ToArray())
  $exitCode = $LASTEXITCODE
} finally {
  if ($savedCodePage -and $savedCodePage -ne '65001') {
    & chcp.com $savedCodePage > $null
  }
}
exit $exitCode
