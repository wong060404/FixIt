# Builds FixIt with the portable MinGW-w64 GCC.
#
#   .\tools\windows\build_fixit.ps1
#
# The repository is found by walking up from this script, so it runs from
# anywhere inside the checkout, and every path below is repository-relative,
# so the checkout itself can live anywhere (any drive, any user, any folder
# name without whitespace issues).  The compiler is looked up in this order:
#   1. -CompilerBin
#   2. $env:FIXIT_TOOLCHAIN\mingw\mingw64\bin
#   3. <repository parent>\toolchain\mingw\mingw64\bin  (fetch_mingw.py)
#   4. $env:TEMP\fixit-toolchain\mingw\mingw64\bin
#   5. the directory of a g++ already on PATH
#   6. C:\msys64\{ucrt64,mingw64,mingw32}\bin
# No environment variable has to be set for a normal run: an installed MSYS2
# or a downloaded portable toolchain in any of those places is enough.
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

param(
  [string]$CompilerBin = ''
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
# Compiler discovery lives in tools/windows/fixit-env.ps1, so the launcher
# (fixit.ps1) and this build script agree on which compiler to use without the
# user setting FIXIT_TOOLCHAIN or prepending anything to PATH.  It accepts both
# winlibs archive layouts (mingw64\bin and mingw\mingw64\bin) and an installed
# MSYS2, so a fresh clone builds on more machines.
. (Join-Path $PSScriptRoot 'fixit-env.ps1')

$mingw = Find-FixitCompilerBin -Repo $repo -Preferred $CompilerBin
if (-not $mingw) {
  Write-Host 'compiler not found. Looked for gcc.exe, g++.exe and ar.exe in:'
  if ($CompilerBin) { Write-Host "  $CompilerBin (-CompilerBin)" }
  Write-Host '  $env:FIXIT_TOOLCHAIN\{mingw64\bin, mingw\mingw64\bin} (if set)'
  Write-Host "  $(Join-Path (Split-Path -Parent $repo) 'toolchain')\..."
  Write-Host "  $(Join-FixitPath $env:TEMP 'fixit-toolchain')\..."
  Write-Host '  the directory of a g++ on PATH'
  Write-Host "  $(Join-Path $env:SystemDrive 'msys64')\{ucrt64,mingw64,mingw32}\bin"
  Write-Host 'download a portable one first with:'
  Write-Host "  python `"$(Join-Path $repo 'tools\windows\fetch_mingw.py')`""
  throw 'no toolchain'
}
$gcc = Join-Path $mingw 'gcc.exe'
$gpp = Join-Path $mingw 'g++.exe'
$ar = Join-Path $mingw 'ar.exe'

# MSYS2's gcc looks up as.exe/ld.exe through PATH and exits 1 without any
# message when its own bin directory is not there (verified: a bare absolute
# path to gcc.exe compiles nothing).  Prepend it for this process only -- a
# script cannot change the environment of the shell that called it.
if ($env:PATH -notlike "*$mingw*") { $env:PATH = "$mingw;$env:PATH" }

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
