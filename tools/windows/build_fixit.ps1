# Builds FixIt with the portable MinGW-w64 GCC.
#
#   .\tools\windows\build_fixit.ps1
#
# The repository is found by walking up from this script, so it runs from
# anywhere inside the checkout.  The toolchain is looked up in this order:
#   1. $env:FIXIT_TOOLCHAIN
#   2. <repository parent>\toolchain     (where fetch_mingw.py puts it)
#   3. $env:TEMP\fixit-toolchain
#
# Why not the documented `cmake -B build && cmake --build build` on Windows:
#   * ninja blocks forever -- it reads each child's output pipe to EOF, and in
#     some environments that pipe never closes even after the child has exited;
#   * mingw32-make reads the UTF-8 Makefiles CMake writes in the local code page,
#     so a non-ASCII path arrives as mojibake and every CreateProcess fails;
#   * the toolchain itself must sit on an ASCII path: gcc derives its own library
#     directory from its executable location and hands those absolute paths to
#     the linker, which decodes them locally.
# Every command below therefore uses relative paths with the repository as the
# working directory.  C sources go through gcc (C11), C++ through g++ (C++20),
# keeping the project's own -Wall -Wextra -Werror gate on its own code.

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
$tc = if ($env:FIXIT_TOOLCHAIN) { $env:FIXIT_TOOLCHAIN }
      else { Join-Path (Split-Path -Parent $repo) 'toolchain' }
if (-not (Test-Path (Join-Path $tc 'mingw\mingw64\bin\g++.exe'))) {
  $fallback = Join-Path $env:TEMP 'fixit-toolchain'
  if (Test-Path (Join-Path $fallback 'mingw\mingw64\bin\g++.exe')) {
    $tc = $fallback
  } else {
    Write-Host 'compiler not found. Looked in:'
    Write-Host "  $tc"
    Write-Host "  $fallback"
    Write-Host 'download it first with:'
    Write-Host "  python `"$(Join-Path $repo 'tools\windows\fetch_mingw.py')`""
    throw 'no toolchain'
  }
}
$mingw = Join-Path $tc 'mingw\mingw64\bin'
$gcc = Join-Path $mingw 'gcc.exe'
$gpp = Join-Path $mingw 'g++.exe'
$ar = Join-Path $mingw 'ar.exe'

$build = Join-Path $repo 'build-win'
New-Item -ItemType Directory -Force -Path (Join-Path $build 'obj'), (Join-Path $build 'bin') | Out-Null

function Invoke-Tool {
  param([string]$Exe, [string[]]$ToolArgs, [string]$Step)
  foreach ($argument in $ToolArgs) {
    if ($argument -match '\s') { throw "argument contains whitespace, quoting not handled: $argument" }
  }
  $info = New-Object System.Diagnostics.ProcessStartInfo
  $info.FileName = $Exe
  $info.Arguments = ($ToolArgs -join ' ')
  $info.WorkingDirectory = $repo
  $info.UseShellExecute = $false
  $info.RedirectStandardOutput = $false
  $info.RedirectStandardError = $false
  $proc = [System.Diagnostics.Process]::Start($info)
  $proc.WaitForExit()
  if ($proc.ExitCode -ne 0) { throw "build step failed ($($proc.ExitCode)): $Step" }
  return $proc.ExitCode
}

function Get-RepoRelative {
  param([string]$FullPath)
  return $FullPath.Substring($repo.Length + 1).Replace('\', '/')
}

# Third-party headers are marked as system includes, exactly as CMake does, so
# -Werror never fires on their diagnostics.
$systemIncludes = @(
  '-isystem', 'third_party/json/include',
  '-isystem', 'third_party/cpp-httplib',
  '-isystem', 'third_party/tree-sitter/lib/include',
  '-isystem', 'third_party/tree-sitter-cpp/src'
)
$fixitDefines = @(
  '-DFIXIT_VERSION="0.1.0"',
  '-DCPPHTTPLIB_OPENSSL_SUPPORT=0',
  '-D_WIN32_WINNT=0x0A00',
  '-DWINVER=0x0A00'
)

# --- tree-sitter runtime + grammar (C11) ------------------------------------
$cSources = @()
$cSources += Get-ChildItem "$repo\third_party\tree-sitter\lib\src\*.c" |
  Where-Object { $_.Name -ne 'lib.c' } | ForEach-Object { $_.FullName }
$cSources += "$repo\third_party\tree-sitter-cpp\src\parser.c"
$cSources += "$repo\third_party\tree-sitter-cpp\src\scanner.c"

$objects = @()
$index = 0
foreach ($source in $cSources) {
  $index++
  $relative = Get-RepoRelative $source
  Write-Host "CC  $relative"
  $object = "build-win/obj/c{0:d2}_{1}.o" -f $index, (Split-Path -Leaf $source)
  Invoke-Tool -Exe $gcc -Step "compile $relative" -ToolArgs @(
    '-std=c11', '-O2', '-w',
    '-Ithird_party/tree-sitter/lib/include',
    '-Ithird_party/tree-sitter/lib/src',
    '-Ithird_party/tree-sitter-cpp/src',
    '-c', $relative, '-o', $object
  ) | Out-Null
  $objects += $object
}

# --- libfixit.a (C++20, with the project's own warning gate) -----------------
$libraryObjects = @()
$index = 0
foreach ($source in Get-ChildItem "$repo\src\*.cpp") {
  $index++
  $relative = Get-RepoRelative $source.FullName
  Write-Host "CXX $relative"
  $object = "build-win/obj/lib{0:d2}_{1}.o" -f $index, $source.BaseName
  Invoke-Tool -Exe $gpp -Step "compile $relative" -ToolArgs (@(
    '-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror',
    '-Iinclude', '-c', $relative, '-o', $object
  ) + $systemIncludes + $fixitDefines) | Out-Null
  $libraryObjects += $object
}
Invoke-Tool -Exe $ar -Step 'archive libfixit.a' -ToolArgs (@('rcs', 'build-win/libfixit.a') + $libraryObjects) | Out-Null
Write-Host 'AR  build-win/libfixit.a'

# --- fixit CLI ---------------------------------------------------------------
Write-Host 'CXX tools/fixit-cli/main.cpp'
Invoke-Tool -Exe $gpp -Step 'link fixit' -ToolArgs (@(
  '-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror', '-static',
  '-Iinclude'
) + $systemIncludes + @('tools/fixit-cli/main.cpp') + $fixitDefines + @(
  'build-win/libfixit.a'
) + $objects + @(
  '-o', 'build-win/bin/fixit.exe', '-lws2_32'
)) | Out-Null
Write-Host "built: $(Join-Path $build 'bin\fixit.exe')"

# --- fixit-matrix (the sweep behind README section 8a) ----------------------
Write-Host 'CXX tools/matrix.cpp'
Invoke-Tool -Exe $gpp -Step 'link fixit-matrix' -ToolArgs (@(
  '-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror', '-static',
  '-Iinclude'
) + $systemIncludes + @('tools/matrix.cpp') + $fixitDefines + @(
  'build-win/libfixit.a'
) + $objects + @(
  '-o', 'build-win/bin/fixit-matrix.exe', '-lws2_32'
)) | Out-Null
Write-Host "built: $(Join-Path $build 'bin\fixit-matrix.exe')"
