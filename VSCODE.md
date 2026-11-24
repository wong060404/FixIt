# Using FixIt from VS Code

> **English** | [繁體中文](VSCODE.zh-TW.md)

FixIt ships with a `.vscode/` folder, so a checkout is a working VS Code project as soon as you open it:
tasks to build and run it, launch configurations to debug it, and settings so IntelliSense and
formatting behave. This page explains what works out of the box on Windows, macOS and Linux, and what
still needs a step.

If you only want the plain terminal workflow, read [`WINDOWS-SETUP.md`](WINDOWS-SETUP.md) (or the
[README](README.md) for CMake on macOS/Linux) instead — this page is the editor-integrated version.

---

## 0. What works where

| | Windows | macOS / Linux |
|---|---|---|
| `windows: build` task (no CMake) | ✅ out of the box | ❌ Windows-only |
| `cmake: configure` / `cmake: build` tasks | ⚠️ needs CMake, see §6 | ✅ out of the box |
| `run:` tasks (`build/bin/fixit`) | ❌ that binary is the CMake build | ✅ after `cmake: build` |
| `windows: run …` tasks (`build-win/bin/fixit.exe`) | ✅ after `windows: build` | ❌ |
| `test:` tasks (6 test binaries + `ctest`) | ⚠️ needs the CMake build, §6 | ✅ after `cmake: build` |
| F5 debugging | ⚠️ needs a Debug build + one extra config, §5 | ✅ after `cmake: build` |
| IntelliSense (`compile_commands.json`) | ⚠️ after §6 | ✅ after `cmake: configure` |

The short version: **on Windows the shipped tasks build and run FixIt, but tests and debugging want a
CMake build; on macOS and Linux everything is CMake.**

---

## 1. Open the repository as the folder

```bash
git clone https://github.com/wong060404/FixIt.git
code FixIt
```

or **File → Open Folder…** and pick the checkout root.

Open the **repository root**, not its parent: every task and launch entry is anchored at
`${workspaceFolder}`, so opening a folder above the checkout makes them point at the wrong paths.

Once the folder is open, the tasks appear under **Terminal → Run Task…** and the configurations under
**Run and Debug**.

> After a `git pull` that touches `.vscode/`, run **Developer: Reload Window** so VS Code picks up the
> new tasks.

---

## 2. The integrated terminal — the fastest path

**View → Terminal** (`` Ctrl+` ``). This is the same CLI as anywhere else; nothing in it is
VS Code-specific, which is why it is the most reliable way to drive FixIt while you iterate.

### Build once

**Windows**

```powershell
.\setup.cmd          # compiler + build + register `fixit` on PATH (once per machine)
# or, without registering anything:
powershell -ExecutionPolicy Bypass -File tools\windows\build_fixit.ps1
```

**macOS / Linux**

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
```

### Then run it

**Windows** — `build-win\bin\fixit.exe`, or `fixit.cmd` if `setup.cmd` registered it:

```powershell
.\build-win\bin\fixit.exe examples\buggy\e1_missing_include.cpp --agent --llm mock --verbose --no-write
```

**macOS / Linux** — `build/bin/fixit`:

```bash
./build/bin/fixit examples/buggy/e1_missing_include.cpp --agent --llm mock --verbose --no-write
```

You should end on `✔ Fixed …` with exit code 0. `--llm mock` is the offline stand-in, so this needs no
network, no key and no model.

Two habits worth keeping:

* **Start with `--no-write`.** It repairs a scratch copy, so you can read the intended diff before
  anything touches your file. Drop the flag when you mean it.
* **`--outline` costs nothing.** `fixit <file> --outline` lists functions, includes and the lines with
  syntax errors without compiling anything.

---

## 3. The tasks that ship with the repo

`.vscode/tasks.json` is checked in, so these exist in every clone. Labels are partly localised in the
file — the important part is the prefix.

### macOS / Linux

| Task | What it does |
|---|---|
| `cmake: configure` | `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug` |
| `cmake: build` | `cmake --build build --parallel` (the default build task) |
| `cmake: clean rebuild` | the same with `--clean-first` |
| `run: fixit --help` | prints the CLI usage |
| `run: fixit outline…` | `--outline` on the file in the editor |
| `run: fixit --no-write…` | the repair loop on a scratch copy |
| `test: …` (7 tasks) | `ctest` and the six individual test binaries |

### Windows

| Task | What it does |
|---|---|
| `windows: build (build-win, no CMake)` | runs `tools/windows/build_fixit.ps1` |
| `windows: run fixit --help` | prints the CLI usage |
| `windows: run fixit outline…` | `--outline` on the file in the editor, from `build-win\` |
| `windows: run fixit --no-write…` | the mock repair loop on a scratch copy |

**Two things to know before you press anything.**

1. **`Ctrl+Shift+B` is not the Windows build.** The default build task is `cmake: build`; on a Windows
   machine without CMake it fails immediately. Use **Terminal → Run Task… → `windows: build`** instead.
2. **The `windows: run …` tasks rebuild first.** They `dependsOn` `windows: build`, and
   `build_fixit.ps1` recompiles every source unconditionally — so each run costs 1–3 minutes before
   FixIt even starts. While you are iterating, run `.\build-win\bin\fixit.exe` directly in the
   terminal (§2) and keep the tasks for the first build.

If a `run:` task seems to do nothing, check that a **saved** file is open in the editor: the tasks pass
`${file}`.

---

## 4. Add a task for your own file

The shipped tasks use `${file}`, so they already work on whatever is open. When you want your own flags
— a model, a trace, extra `-I` paths — add a task in `.vscode/tasks.json`:

```json
{
  "label": "fixit: repair this file (local model)",
  "type": "shell",
  "command": "${workspaceFolder}/build/bin/fixit",
  "args": [
    "${file}",
    "--agent", "--llm", "openai",
    "--base-url", "http://127.0.0.1:11434/v1",
    "--model", "qwen2.5-coder:7b",
    "--api-key", "ollama",
    "--iterations", "4", "--verbose", "--no-write"
  ],
  "options": { "cwd": "${fileDirname}" },
  "problemMatcher": []
}
```

On Windows, point `command` at `${workspaceFolder}\\build-win\\bin\\fixit.exe` and add
`"--compiler", "C:\\path\\to\\mingw64\\bin\\g++.exe"` so the task does not depend on a `PATH` set in
some other terminal.

> `.vscode/tasks.json` is tracked. A task you add for your machine will show up in `git status`;
> keep that out of pull requests.

---

## 5. Debugging with F5

`.vscode/launch.json` ships nine `cppdbg` configurations: three for the CLI and one per test binary.
They launch `${workspaceFolder}/build/bin/…` and run `cmake: build` first.

**Requirements**

* the [C/C++ extension](https://marketplace.visualstudio.com/items?itemName=ms-vscode.cpptools)
  (`ms-vscode.cpptools`) — provides `cppdbg`;
* a debugger: `lldb` on macOS, `gdb` on Linux, `gdb` on Windows (the portable winlibs toolchain and
  MSYS2 both ship one).

**macOS / Linux** — run `cmake: build`, open a source file, press F5, pick a `fixit CLI…` configuration.

**Windows** — two obstacles, both fixable:

1. The configurations point at `build/bin/fixit` (no `.exe`) and at a CMake build that Windows does not
   have by default.
2. `build_fixit.ps1` compiles with `-O2` and no `-g`, so `build-win\bin\fixit.exe` has no symbols worth
   stepping through.

The practical route is a CMake Debug build (§6) and then one extra configuration:

```json
{
  "name": "fixit (Windows, Debug build)",
  "type": "cppdbg",
  "request": "launch",
  "program": "${workspaceFolder}\\build\\bin\\fixit.exe",
  "args": ["${file}", "--outline"],
  "cwd": "${workspaceFolder}",
  "stopAtEntry": false,
  "externalConsole": false,
  "MIMode": "gdb",
  "miDebuggerPath": "C:\\path\\to\\mingw64\\bin\\gdb.exe",
  "preLaunchTask": "cmake: build",
  "setupCommands": [
    { "description": "Enable pretty-printing", "text": "-enable-pretty-printing", "ignoreFailures": true }
  ]
}
```

Set `miDebuggerPath` to a real `gdb.exe` — the one inside your MinGW toolchain, or MSYS2's.

> Do not debug `examples/buggy/e1`–`e3` and expect a clean run: a failure to compile is their designed
> behaviour. Use your own file.

---

## 6. Windows: a CMake build for tests, IntelliSense and debugging

`build_fixit.ps1` builds two binaries (`fixit.exe`, `fixit-matrix.exe`). The **six test binaries only
exist in a CMake build**, and so do `compile_commands.json` (IntelliSense) and `-g` symbols.

Two Windows-specific settings are required in this revision. Both were verified on Windows 11 with the
portable winlibs GCC 16.2.0:

```powershell
cmake -S . -B build -G "MinGW Makefiles" `
  -DCMAKE_BUILD_TYPE=Debug `
  -DCMAKE_MAKE_PROGRAM="C:\path\to\mingw64\bin\mingw32-make.exe" `
  -DCMAKE_CXX_FLAGS="-D_WIN32_WINNT=0x0A00" `
  -DCMAKE_C_FLAGS="-D_WIN32_WINNT=0x0A00"

cmake --build build --parallel
```

* **`-G "MinGW Makefiles"` is mandatory.** The Ninja generator can hang indefinitely on Windows (a
  documented problem in this project — `ninja` reads each child's output pipe until EOF, and in some
  environments that pipe never closes).
* **`-D_WIN32_WINNT=0x0A00` is mandatory.** The vendored `cpp-httplib` refuses to compile with a lower
  Windows version (`httplib.h: #error cpp-httplib doesn't support Windows 8 or lower`), and MinGW
  defaults to an older value. `build_fixit.ps1` passes this define; `CMakeLists.txt` does not yet.

After that build, `build\bin\` contains all eight executables, and the macOS/Linux tasks (`run:…`,
`test:…`) work on Windows too — your machine now matches what they expect. Deleting `build/` means
re-running the command above; `cmake: configure` does not carry those two flags.

---

## 7. Repairing with a real model, from the editor

`--llm mock` repairs four demonstration fault shapes and nothing else. For arbitrary code, point FixIt
at a model.

### A local model — no key, no relay

Anything OpenAI-compatible over plain HTTP works; [Ollama](https://ollama.com/download) is the shortest
path. (`fixit` below means whatever your build produced — `./build/bin/fixit` on macOS/Linux,
`build-win\bin\fixit.exe` on Windows.)

```bash
ollama pull qwen2.5-coder:7b
```

```bash
fixit your_file.cpp --agent --llm openai \
  --base-url http://127.0.0.1:11434/v1 --model qwen2.5-coder:7b --api-key ollama --verbose --no-write
```

### A cloud endpoint — the relay

A default build has no TLS, so `--base-url https://…` fails with *this build has no TLS support*. The
relay (`tools/windows/llm-relay.py`, pure Python, no dependencies) speaks plain HTTP to FixIt and HTTPS
to your endpoint, and keeps the key out of `fixit`'s command line.

Put the key in the git-ignored file — or export it — and start the relay in a second terminal:

```bash
# key
mkdir -p .secrets && printf '%s' 'sk-...' > .secrets/fixit_key      # macOS/Linux
# New-Item -ItemType Directory -Force .secrets; Read-Host | Set-Content -NoNewline .secrets\fixit_key   # Windows

# relay (leave it running)
RELAY_TARGET=https://your-endpoint/v1 python tools/windows/llm-relay.py
# PowerShell: $env:RELAY_TARGET = "https://your-endpoint/v1"; python tools/windows/llm-relay.py
```

Then run FixIt in the first terminal, or from a task:

```bash
fixit your_file.cpp --agent --llm openai \
  --base-url http://127.0.0.1:8791/v1 --model your-model --api-key relay \
  --iterations 4 --verbose --no-write
```

`--api-key relay` is a placeholder — any non-empty string; the relay injects the real credential. Check
the endpoint before a long run:

```bash
python tools/windows/probe-endpoint.py "https://your-endpoint/v1" "your-model"
```

A `200` and `VERDICT : endpoint reachable, key accepted, model served, tools field accepted` means the
whole chain works. Two rules of thumb: **four iterations** is usually enough (a round that only calls
`read` is normal), and **the model must support tool calling** — if it only ever replies with prose,
`tool_calls` stays empty and nothing gets patched.

---

## 8. Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| Task flashes and does nothing | `${file}` is empty — no file open, or unsaved | open and save a `.cpp` in the editor |
| `'cmake' is not recognized` | CMake missing, or `Ctrl+Shift+B` picked the default CMake task on Windows | install CMake, or use **Run Task… → `windows: build`** |
| `the compiler could not be executed` | no `g++` on `PATH` | pass `--compiler` with a full path, and put that `bin` folder on `PATH` too (`as.exe`/`ld.exe` are found there) |
| `fixit: 'x.cpp' is not a readable file` | relative path resolved against the wrong folder | use `${file}` in a task, or an absolute path |
| `this build has no TLS support` | `--base-url https://…` | use the relay (§7) |
| `RELAY_TARGET is not set.` | relay started without the variable | set it, then restart the relay |
| `HTTP 401` / `404` | wrong key / wrong endpoint or model name | run `probe-endpoint.py` first |
| `cannot listen on 127.0.0.1:8791` | another relay is already running | use it, or pass `RELAY_PORT` / `-Port` |
| `ninja` hangs during configure | Ninja generator on Windows | `-G "MinGW Makefiles"` (§6) |
| `httplib.h:30:2: error: #error` | `_WIN32_WINNT` too low | add `-D_WIN32_WINNT=0x0A00` (§6) |
| F5: *program '…/build/bin/fixit' does not exist* | no CMake build yet, and the path lacks `.exe` on Windows | build with CMake (§6), then use the Windows config in §5 |
| Output is mojibake (`鈺愨晲`) | terminal is not UTF-8 | `chcp 65001` (Windows), or use the launcher, which does it for you |

Exit codes are `0` clean or repaired, `1` not repaired, `2` usage error, `3` internal error. A compiler
that cannot be started is reported as `the compiler could not be executed` and exits `1`.

---

## 9. Extensions worth having

| Extension | Why |
|---|---|
| `ms-vscode.cpptools` | required for F5 (`cppdbg`) and for IntelliSense in this project |
| `ms-vscode.cmake-tools` | optional; gives a status bar and `CMake: Configure`, but the checked-in tasks never need it |

The repository's `.vscode/settings.json` already points `compile_commands.json` at `build/`, so
IntelliSense resolves the vendored headers (tree-sitter, Catch2, `httplib`) as soon as a CMake
configure has run.
