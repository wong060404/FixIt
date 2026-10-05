> **English** | [繁體中文](WINDOWS-SETUP.zh-TW.md)

# FixIt on Windows — complete guide (clone, build, run)

> From `git clone` to your first repaired bug. Every step says what to type and what you
> should see. No prior C++, compiler or Git knowledge is assumed.
>
> Every command below was run on Windows 11 with a Traditional Chinese locale and
> PowerShell.

---

## 0. Where this guide takes you

| Stage | What you end up with | Time |
|---|---|---|
| Install (once) | The program compiled into `fixit.exe`, plus a compiler it can drive | 10–25 min |
| Use | Find and fix errors in C++ files | seconds to minutes each |
| (Optional) Model backend | Automatic repair of arbitrary bugs, using your own API or a local model | 5 min |

When you are done, your machine has:

```
C:\FixIt\                          <- work folder (plain ASCII path: important!)
├── FixIt\                         <- the project you cloned
│   ├── build-win\bin\fixit.exe    <- the tool itself
│   └── tools\windows\             <- helper scripts used by this guide
└── toolchain\                     <- the compiler (unpacked, nothing installed)
```

---

## Before you start: three rules

**Rule 1: the whole path must be plain ASCII — no Chinese characters, no spaces.**
Use `C:\FixIt`. On Windows, gcc hands its own library directory (an absolute path) to
the linker, which decodes it in the local code page; a non-ASCII path turns into
mojibake and you get a pile of confusing `No such file or directory` errors. Getting
this wrong is the most painful way to fail.

**Rule 2: do not put it in OneDrive, on the Desktop, or under Documents.**
The compiler plus build output is roughly 2 GB, which OneDrive will try to sync; and its
Files On-Demand feature can turn the built `.exe` into a cloud placeholder that cannot
run.

**Rule 3: FixIt needs a real C++ compiler.**
Its whole job is compile → read errors → patch → compile again. Step 3 downloads a
portable GCC for you; you do not need Visual Studio.

---

## Part 1 — Install (do this once)

### Step 1: check Git and Python

Press **Win**, type `powershell`, press **Enter**. Every command below goes in that window.

```powershell
git --version
python --version
```

**You should see** two version lines, for example:

```
git version 2.45.1.windows.1
Python 3.12.10
```

If either says "not recognized":

* **Git** → install from <https://git-scm.com/download/win>, clicking Next all the way.
* **Python** → install from <https://www.python.org/downloads/windows/>. **Tick
  `Add python.exe to PATH` on the first installer screen.**

Close PowerShell, open it again, and check once more.

### Step 2: create the work folder and clone FixIt

```powershell
New-Item -ItemType Directory -Force -Path C:\FixIt | Out-Null
Set-Location C:\FixIt
git clone https://github.com/wong060404/FixIt.git
```

**You should see**:

```
Cloning into 'FixIt'...
Resolving deltas: 100% (.../...), done.
```

Check it:

```powershell
Test-Path C:\FixIt\FixIt\CMakeLists.txt
```

**You should see** `True`.

> Without Git: on the GitHub page press the green **Code** button → **Download ZIP**,
> unpack it, rename the folder to `FixIt`, and put it in `C:\FixIt\`.
>
> **Nothing has to be patched or copied after cloning** — the Windows support and all
> helper scripts used by this guide are already in the repository.

### Step 3: download the compiler (about 274 MB)

```powershell
python C:\FixIt\FixIt\tools\windows\fetch_mingw.py
```

This downloads the winlibs MinGW-w64 GCC from GitHub and unpacks it into
`C:\FixIt\toolchain\` (about 1.5 GB unpacked).

**You should see** (last lines):

```
repository : C:\FixIt\FixIt
destination: C:\FixIt\toolchain
asset      : winlibs-x86_64-posix-seh-gcc-...zip (... MB)
   ... 50 MB
downloaded -> C:\FixIt\FixIt\tools\windows\.cache\winlibs-...zip
extracting (about 1.5 GB, this takes a minute) ...
extracted  -> C:\FixIt\toolchain
compiler   : C:\FixIt\toolchain\mingw\mingw64\bin\g++.exe
```

Check it:

```powershell
& C:\FixIt\toolchain\mingw\mingw64\bin\g++.exe --version
```

**You should see** a first line like:

```
g++.exe (MinGW-W64 x86_64-ucrt-posix-seh, built by Brecht Sanders, r2) 16.2.0
```

> You can delete the zip under `C:\FixIt\FixIt\tools\windows\.cache\` afterwards to save
> 274 MB. Re-running the script restores it if needed.

### Step 4: build FixIt (about 1–3 minutes)

```powershell
powershell -ExecutionPolicy Bypass -File C:\FixIt\FixIt\tools\windows\build_fixit.ps1
```

(`-ExecutionPolicy Bypass` only allows this script to run for that one command; it does
not change any system setting.)

**You should see** a run of `CC ...` and `CXX ...` lines, ending with:

```
AR  build-win/libfixit.a
CXX tools/fixit-cli/main.cpp
built: C:\FixIt\FixIt\build-win\bin\fixit.exe
CXX tools/matrix.cpp
built: C:\FixIt\FixIt\build-win\bin\fixit-matrix.exe
```

Check it:

```powershell
Test-Path C:\FixIt\FixIt\build-win\bin\fixit.exe
```

**You should see** `True`.

### Step 5: the first run (no network, no account, no key)

```powershell
chcp 65001 > $null
$env:PATH = "C:\FixIt\toolchain\mingw\mingw64\bin;$env:PATH"
$env:NO_COLOR = "1"
Set-Location C:\FixIt\FixIt
.\build-win\bin\fixit.exe examples\buggy\e1_missing_include.cpp --agent --llm mock --verbose --no-write --no-timing
```

**You should see** (last lines):

```
── iteration 2 ----------------------------
$ g++ -fsyntax-only e1_missing_include.cpp
  ✗ 1 error
    [E1]   L12   expected ',' or ';' before 'return'
  → context: fn parse_count() [L10–L13]
  → patch…
    ✓ hunk 1 @ L11   (fuzzy, drift-1)
$ g++ -fsyntax-only e1_missing_include.cpp
  ✓ clean
✔ Fixed 10 errors in 2 iterations
```

**Seeing `✔ Fixed ...` means the installation worked.** What you just watched is a
complete repair loop:

| Output | Meaning |
|---|---|
| `✗ 1 error` / `[E1] L12 …` | FixIt ran the real compiler and turned its errors into data |
| `→ context: fn parse_count() [L10–L13]` | tree-sitter located which function the error is in |
| `→ patch…` → `✓ hunk 1 @ L11` | the patch was applied; `fuzzy, drift-1` means the line number was off by one and the tolerance window rescued it |
| `✓ clean` | recompiled: only the compiler is allowed to declare success |
| `✔ Fixed 10 errors in 2 iterations` | the result (exit code 0) |

> `--no-write` repairs a scratch copy and leaves your file untouched. Drop the flag to
> repair in place.
> `--llm mock` is the built-in offline stand-in: it only knows four demonstration fault
> shapes. To repair anything of your own, see Part 2.

---

## Part 2 — Using it

In every new PowerShell window, run these three lines first (or make them permanent, see
Appendix B):

```powershell
chcp 65001 > $null
$env:PATH = "C:\FixIt\toolchain\mingw\mingw64\bin;$env:PATH"
$env:NO_COLOR = "1"
```

| What those lines do | Why |
|---|---|
| `chcp 65001` | switches the console to UTF-8; without it FixIt's box-drawing characters show as mojibake like `鈺愨晲` (cosmetic only) |
| `PATH` | lets fixit find `g++`. Alternatively pass `--compiler "C:\FixIt\toolchain\mingw\mingw64\bin\g++.exe"` every time |
| `NO_COLOR` | turns off colour so output is easier to copy |

### Use A: just find out what is wrong (fastest, no model)

```powershell
C:\FixIt\FixIt\build-win\bin\fixit.exe "C:\your-project\your-file.cpp"
```

**You should see** every error with its line, column and message.

### Use B: see the structure of a file

```powershell
C:\FixIt\FixIt\build-win\bin\fixit.exe "C:\your-project\your-file.cpp" --outline
```

It lists the functions and their line ranges, the includes, and which lines have syntax
errors — handy for getting oriented in unfamiliar code.

### Use C: let a model repair it

FixIt speaks to any **OpenAI-compatible** endpoint. Pick one of the two:

#### C-1. A local model (simplest: no key, no relay)

Install [Ollama](https://ollama.com/download), then pull a model that supports tool
calling:

```powershell
ollama pull qwen2.5-coder:7b
```

Point FixIt straight at it (Ollama is plain http, so no relay is needed):

```powershell
C:\FixIt\FixIt\build-win\bin\fixit.exe "C:\your-project\your-file.cpp" `
  --agent --llm openai `
  --base-url http://127.0.0.1:11434/v1 --model qwen2.5-coder:7b --api-key ollama `
  --verbose --no-write
```

#### C-2. Your own cloud API over https (needs the local relay)

There is exactly one reason for the relay: **this build has no TLS**, so fixit itself
cannot reach `https://`. The relay lets fixit speak plain http to localhost while the
relay talks https to your endpoint with your key. A useful side effect: **your key never
appears on fixit's command line**.

**Put the key in place first** (this location is already excluded by `.gitignore`, so it
cannot be committed):

```powershell
New-Item -ItemType Directory -Force -Path C:\FixIt\FixIt\.secrets | Out-Null
Read-Host -Prompt "Paste your API key and press Enter" | Set-Content -NoNewline C:\FixIt\FixIt\.secrets\fixit_key
```

**Check the endpoint, key and model first** (a few seconds, saves a lot of guessing):

```powershell
python C:\FixIt\FixIt\tools\windows\probe-endpoint.py "https://your-endpoint/v1" "your-model-name"
```

**You should see**:

```
HTTP     : 200
VERDICT  : endpoint reachable, key accepted, model served, tools field accepted
```

`HTTP : 401` means the key is wrong, `404` means the endpoint path or model name is
wrong, `transport error` means the network is blocked.

**Then use two windows.**

Window A (the relay — leave it running):

```powershell
$env:RELAY_TARGET = "https://your-endpoint/v1"
python C:\FixIt\FixIt\tools\windows\llm-relay.py
```

**You should see**:

```
relay      : http://127.0.0.1:8791  ->  https://your-endpoint/v1
key loaded : yes
```

> **While the relay runs, your key sits in its memory**, so press **Ctrl+C** when you are
> done. If it says `cannot listen on 127.0.0.1:8791`, another relay is already running:
> just use that one, or stop the old one first.

Window B (let FixIt repair your file):

```powershell
chcp 65001 > $null
$env:PATH = "C:\FixIt\toolchain\mingw\mingw64\bin;$env:PATH"
$env:NO_COLOR = "1"

C:\FixIt\FixIt\build-win\bin\fixit.exe "C:\your-project\your-file.cpp" `
  --agent --llm openai `
  --base-url http://127.0.0.1:8791/v1 --model "your-model-name" --api-key relay `
  --verbose --no-write
```

**Always start with `--no-write`**: it repairs a scratch copy so you can see what the
model intends before anything is written. When you are happy, run it again without
`--no-write` to repair the file in place.

**You should see** something like (a file with a case-typo, as an example):

```
── iteration 1 ──  ✗ 1 error  [E1] L6 'S' was not declared in this scope
                  → context: fn main() [L4–L8]
── iteration 2 ──  → patch…  ✓ hunk 1 @ L6  (exact)
                  ✓ clean
✔ Fixed 1 errors in 2 iterations (3.5s)
```

The model may call `read` first to look at more of the file, so a round without `patch`
is normal.

#### C-3. One command for the whole loop (optional)

To see the per-round log, the metrics and a before/after diff, use this script — it
starts the relay itself, runs fixit, and stops the relay afterwards:

```powershell
powershell -ExecutionPolicy Bypass `
  -File C:\FixIt\FixIt\tools\windows\run-real-loop.ps1 `
  -BaseUrl "https://your-endpoint/v1" -Model "your-model-name" -File "C:\your-project\your-file.cpp"
```

Without `-File` it creates a small file with one deliberate bug in
`%TEMP%\fixit-demo\demo_bug.cpp` and repairs that, so you can try the whole thing with
nothing prepared.

### Flags worth knowing

| Flag | Effect |
|---|---|
| `--agent` | run the repair loop; without it, compile once and list diagnostics |
| `--llm mock` / `--llm openai` | model backend; `mock` is the default |
| `--base-url` / `--model` / `--api-key` | endpoint, model, key (the key can also come from `FIXIT_API_KEY`) |
| `--no-write` | repair a scratch copy, leaving your file untouched (start here) |
| `--verbose` | stream diagnostics, location and patch positions per round |
| `--trace PATH` / `--metrics PATH` | write a JSON trace / statistics (which tool the model called, patch success rate) |
| `--iterations N` | maximum repair rounds (default 4) |
| `--compiler NAME` | compiler to drive (default `g++`) |
| `-I DIR` / `-D NAME[=VALUE]` / `--flag FLAG` | passed through to the compiler, repeatable (needed when the file includes project headers) |
| `--outline` | print the structure only, no compile |
| `-h` / `--help` | usage |

Exit codes: `0` clean or repaired, `1` not repaired, `2` usage error, `3` internal error,
`127` compiler not found (PATH or `--compiler` wrong).

---

## Troubleshooting

| Message you see | Cause | Fix |
|---|---|---|
| `'git' is not recognized` | Git not installed | Step 1 |
| `'python' is not recognized` | Python missing, or PATH not ticked during install | Step 1, then reopen the window |
| `fetch_mingw.py` says the destination contains non-ASCII | the work path has Chinese characters | use `C:\FixIt` and clone again |
| `No such file or directory`, `cannot find -lstdc++` while building | the toolchain sits under a non-ASCII path | move the whole `C:\FixIt` to a plain ASCII path |
| `compiler not found. Looked in: ...` | the compiler has not been downloaded | do Step 3 |
| `the compiler could not be executed (exit code 127)` | `g++` is not on PATH and `--compiler` was not given | run the three lines at the top of Part 2 |
| `this build has no TLS support; rebuild with -DFIXIT_ENABLE_OPENSSL=ON` | `--base-url` points straight at `https://` | use the relay from C-2, or the local model from C-1 |
| `RELAY_TARGET is not set.` | the relay was not told where to connect | `$env:RELAY_TARGET = "https://your-endpoint/v1"` first |
| `cannot listen on 127.0.0.1:8791` / `port 8791 is already serving a relay for a different endpoint` | another relay is already running | use it, or stop it, or pass `-Port 8792` |
| `LLM request returned HTTP 401` / `404` | wrong key / wrong endpoint or model name | check with `probe-endpoint.py` first |
| the relay prints `key loaded : no` | the key file is not at `C:\FixIt\FixIt\.secrets\fixit_key` | see C-2 |
| `tool_calls` is always empty, the model only returns text | the model does not support tool calling | use a model that supports function calling |
| output looks like `鈺愨晲` or `鉁?` | the console is not UTF-8 | run `chcp 65001` |
| running a `.ps1` says "running scripts is disabled on this system" | PowerShell blocks scripts by default | add `-ExecutionPolicy Bypass`, as in Step 4 |
| `PATH` stops working after closing the window | environment variables are per-window | see Appendix B |

---

## Appendix A — why all this is necessary

**Why download a separate compiler?**
FixIt's purpose is to drive a real compiler and interpret its output, so it needs one
both to build itself and to be the compiler it drives. The winlibs MinGW-w64 GCC needs no
installation and no administrator rights, and its diagnostic format is one the project
explicitly supports.

**Why must the path be ASCII?**
gcc passes the library paths under its own installation directory to the linker `ld`.
With a non-ASCII path those bytes are re-decoded in the local code page (936/GBK on a
Chinese Windows), so `crt2.o` and `-lstdc++` are all "not found" — and the error message
never points at the real cause. `C:\FixIt\toolchain` avoids the whole class of problem.

**Why doesn't the Windows build use the project's `cmake`?**
Two independent problems were hit here. `ninja` blocks forever: it reads each child's
output pipe until EOF, and in some environments that pipe never closes. `mingw32-make`
reads the UTF-8 Makefiles CMake writes in the local code page, so a non-ASCII path
arrives as mojibake and every process launch fails. `build_fixit.ps1` does exactly what
CMake would do — C via gcc, C++ via g++, the project's own `-Wall -Wextra -Werror` on its
own code — just without CMake. Linux and macOS users can follow the project README's
`cmake` flow.

**Why is there a local relay?**
The project deliberately avoids an OpenSSL dependency (decision ADR-004), so the default
build has no TLS. The relay uses Python's own certificate handling for https while FixIt
speaks plain http. If you want FixIt itself to reach https, build with
`-DFIXIT_ENABLE_OPENSSL=ON` and, on Windows, point `FIXIT_CA_BUNDLE` at a CA bundle.

**Why `--no-write`?**
Because FixIt repairs files in place by default. Looking at the intended change first,
then applying it, is simply safer.

## Appendix B — make PATH permanent (optional)

So you do not have to type those three lines in every new window:

```powershell
[Environment]::SetEnvironmentVariable(
  "Path",
  "C:\FixIt\toolchain\mingw\mingw64\bin;" + [Environment]::GetEnvironmentVariable("Path","User"),
  "User")
[Environment]::SetEnvironmentVariable("FIXIT_TOOLCHAIN", "C:\FixIt\toolchain", "User")
```

**Open a new PowerShell window for it to take effect.** (This changes only your own user
settings, never the system ones.)

## Appendix C — every command in one place

```powershell
# --- install (once) ---------------------------------------------------------
New-Item -ItemType Directory -Force -Path C:\FixIt | Out-Null
Set-Location C:\FixIt
git clone https://github.com/wong060404/FixIt.git
python C:\FixIt\FixIt\tools\windows\fetch_mingw.py
powershell -ExecutionPolicy Bypass -File C:\FixIt\FixIt\tools\windows\build_fixit.ps1

# --- every new window -------------------------------------------------------
chcp 65001 > $null
$env:PATH = "C:\FixIt\toolchain\mingw\mingw64\bin;$env:PATH"
$env:NO_COLOR = "1"

# --- find what is wrong -----------------------------------------------------
C:\FixIt\FixIt\build-win\bin\fixit.exe "C:\path\to\file.cpp"
C:\FixIt\FixIt\build-win\bin\fixit.exe "C:\path\to\file.cpp" --outline

# --- offline demonstration (four fault shapes only) -------------------------
C:\FixIt\FixIt\build-win\bin\fixit.exe "C:\path\to\file.cpp" --agent --llm mock --verbose --no-write

# --- repair with a local model (Ollama, no key) -----------------------------
C:\FixIt\FixIt\build-win\bin\fixit.exe "C:\path\to\file.cpp" --agent --llm openai `
  --base-url http://127.0.0.1:11434/v1 --model qwen2.5-coder:7b --api-key ollama --verbose

# --- repair with your own cloud API (relay required) ------------------------
python C:\FixIt\FixIt\tools\windows\probe-endpoint.py "https://your-endpoint/v1" "your-model"
$env:RELAY_TARGET = "https://your-endpoint/v1"
python C:\FixIt\FixIt\tools\windows\llm-relay.py          # window A, keep it open
C:\FixIt\FixIt\build-win\bin\fixit.exe "C:\path\to\file.cpp" --agent --llm openai `
  --base-url http://127.0.0.1:8791/v1 --model "your-model" --api-key relay --verbose --no-write
```
