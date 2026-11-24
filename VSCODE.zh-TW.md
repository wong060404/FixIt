# 在 VS Code 中使用 FixIt

> [English](VSCODE.md) | **繁體中文**

FixIt 附帶一個 `.vscode/` 資料夾，因此 checkout 一打開就是一個可用的 VS Code 專案：內建 build 與 run
的 tasks、用於除錯的 launch 設定，以及讓 IntelliSense 與格式化正常運作的設定。本頁說明 Windows、macOS
與 Linux 上哪些功能開箱即用、哪些仍需要額外步驟。

若只需要純終端機流程，請改讀 [`WINDOWS-SETUP.md`](WINDOWS-SETUP.md)（macOS/Linux 使用 CMake 請見
[README](README.md)）—— 本頁是編輯器整合版。

---

## 0. 各平台支援的功能

| | Windows | macOS / Linux |
|---|---|---|
| `windows: build` task（免 CMake） | ✅ 開箱即用 | ❌ 僅限 Windows |
| `cmake: configure` / `cmake: build` tasks | ⚠️ 需要 CMake，見第 6 節 | ✅ 開箱即用 |
| `run:` tasks（`build/bin/fixit`） | ❌ 該執行檔屬於 CMake build | ✅ `cmake: build` 之後 |
| `windows: run …` tasks（`build-win/bin/fixit.exe`） | ✅ `windows: build` 之後 | ❌ |
| `test:` tasks（6 個 test 執行檔 + `ctest`） | ⚠️ 需要 CMake build，見第 6 節 | ✅ `cmake: build` 之後 |
| F5 除錯 | ⚠️ 需要 Debug build 與一個額外設定，見第 5 節 | ✅ `cmake: build` 之後 |
| IntelliSense（`compile_commands.json`） | ⚠️ 完成第 6 節之後 | ✅ `cmake: configure` 之後 |

一句總結：**Windows 上內建 tasks 可以 build 與 run FixIt，但測試與除錯需要 CMake build；macOS 與
Linux 則全部依賴 CMake。**

---

## 1. 以 repository 根目錄開啟

```bash
git clone https://github.com/wong060404/FixIt.git
code FixIt
```

或使用 **File → Open Folder…** 選擇 checkout 根目錄。

請開啟 **repository 根目錄**，不要開啟它的上一層：所有 task 與 launch 設定都以 `${workspaceFolder}`
為基準，開啟上一層會指向錯誤路徑。

開啟後，tasks 位於 **Terminal → Run Task…**，設定位於 **Run and Debug**。

> 若 `git pull` 改動了 `.vscode/`，請執行 **Developer: Reload Window**，讓 VS Code 重新載入 tasks。

---

## 2. 整合式終端機 —— 最快的做法

**View → Terminal**（`` Ctrl+` ``）。這與其他環境的 CLI 完全相同，沒有任何 VS Code 專屬行為，因此是
迭代時最可靠的方式。

### 先 build 一次

**Windows**

```powershell
.\setup.cmd          # 編譯器 + 建置 + 將 `fixit` 註冊到 PATH（每台機器一次）
# 或者不註冊任何東西：
powershell -ExecutionPolicy Bypass -File tools\windows\build_fixit.ps1
```

**macOS / Linux**

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
```

### 然後執行

**Windows** —— `build-win\bin\fixit.exe`；若 `setup.cmd` 已註冊，也可使用 `fixit.cmd`：

```powershell
.\build-win\bin\fixit.exe examples\buggy\e1_missing_include.cpp --agent --llm mock --verbose --no-write
```

**macOS / Linux** —— `build/bin/fixit`：

```bash
./build/bin/fixit examples/buggy/e1_missing_include.cpp --agent --llm mock --verbose --no-write
```

應該以 `✔ Fixed …` 與 exit code 0 結束。`--llm mock` 是離線假模型，因此不需要網路、不需要 key、也不
需要模型。

兩個值得養成的習慣：

* **先使用 `--no-write`。** 它只修改暫存副本，讓你先看清楚打算怎麼改，再決定是否動到原檔。確定要改
  時再移除該旗標。
* **`--outline` 零成本。** `fixit <file> --outline` 會列出函式、include 與有語法錯誤的行，完全不編譯。

---

## 3. Repo 內建的 tasks

`.vscode/tasks.json` 已 commit，因此每個 clone 都有。部分 label 在檔案中是中文 —— 重點是前綴。

### macOS / Linux

| Task | 功能 |
|---|---|
| `cmake: configure` | `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug` |
| `cmake: build` | `cmake --build build --parallel`（預設 build task） |
| `cmake: clean rebuild` | 同上，加上 `--clean-first` |
| `run: fixit --help` | 印出 CLI 用法 |
| `run: fixit outline…` | 對編輯器中的檔案執行 `--outline` |
| `run: fixit --no-write…` | 對暫存副本執行修復循環 |
| `test: …`（7 個 task） | `ctest` 與 6 個獨立 test 執行檔 |

### Windows

| Task | 功能 |
|---|---|
| `windows: build (build-win, no CMake)` | 執行 `tools/windows/build_fixit.ps1` |
| `windows: run fixit --help` | 印出 CLI 用法 |
| `windows: run fixit outline…` | 由 `build-win\` 對編輯器檔案執行 `--outline` |
| `windows: run fixit --no-write…` | 對暫存副本執行 mock 修復循環 |

**動手之前需要知道兩件事。**

1. **`Ctrl+Shift+B` 不是 Windows build。** 預設 build task 是 `cmake: build`；Windows 機器若沒有
   CMake 會立即失敗。請改用 **Terminal → Run Task… → `windows: build`**。
2. **`windows: run …` 會先重建。** 它們 `dependsOn` `windows: build`，而 `build_fixit.ps1` 每次都
   無條件重新編譯所有 source —— 因此每次 run 之前要等 1–3 分鐘。迭代期間請直接在終端機執行
   `.\build-win\bin\fixit.exe`（第 2 節），tasks 留給第一次 build。

若 `run:` task 似乎沒有任何反應，請確認編輯器中開啟的檔案**已儲存**：tasks 會傳入 `${file}`。

---

## 4. 為自己的檔案新增 task

內建 tasks 使用 `${file}`，因此開啟任何檔案都能直接使用。若要加入自己的旗標（模型、trace、額外
`-I`），請在 `.vscode/tasks.json` 中新增：

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

Windows 上，將 `command` 改為 `${workspaceFolder}\\build-win\\bin\\fixit.exe`，並加入
`"--compiler", "C:\\path\\to\\mingw64\\bin\\g++.exe"`，這樣 task 就不依賴其他終端機設定的 `PATH`。

> `.vscode/tasks.json` 是 tracked 檔案。為自己機器新增的 task 會出現在 `git status`；請不要放進
> pull request。

---

## 5. 使用 F5 除錯

`.vscode/launch.json` 內建 9 個 `cppdbg` 設定：3 個 CLI、以及每個 test 執行檔一個。它們會啟動
`${workspaceFolder}/build/bin/…`，並先執行 `cmake: build`。

**需求：**

* [C/C++ extension](https://marketplace.visualstudio.com/items?itemName=ms-vscode.cpptools)
  （`ms-vscode.cpptools`）—— 提供 `cppdbg`；
* 除錯器：macOS 使用 `lldb`、Linux 使用 `gdb`、Windows 使用 `gdb`（可攜式 winlibs toolchain 與
  MSYS2 都有提供）。

**macOS / Linux** —— 執行 `cmake: build`，開啟一個 source 檔，按 F5，選擇 `fixit CLI…` 設定。

**Windows** —— 有兩個障礙，兩者都可以解決：

1. 設定指向 `build/bin/fixit`（沒有 `.exe`），而且指向 Windows 預設沒有的 CMake build。
2. `build_fixit.ps1` 使用 `-O2` 編譯、沒有 `-g`，因此 `build-win\bin\fixit.exe` 沒有可供 step 的符號。

實務做法是先做 CMake Debug build（第 6 節），再加入一個設定：

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

`miDebuggerPath` 必須指向真正的 `gdb.exe` —— MinGW toolchain 內的，或 MSYS2 的。

> 不要除錯 `examples/buggy/e1`–`e3` 並期待順利通過：編譯失敗就是它們的設計行為。請使用自己的檔案。

---

## 6. Windows：為測試、IntelliSense 與除錯建立 CMake build

`build_fixit.ps1` 只建置兩個執行檔（`fixit.exe`、`fixit-matrix.exe`）。**6 個 test 執行檔只存在於
CMake build**，`compile_commands.json`（IntelliSense）與 `-g` 符號也是如此。

此版本在 Windows 上需要兩個專屬設定。兩者都在 Windows 11 + 可攜式 winlibs GCC 16.2.0 上實測過：

```powershell
cmake -S . -B build -G "MinGW Makefiles" `
  -DCMAKE_BUILD_TYPE=Debug `
  -DCMAKE_MAKE_PROGRAM="C:\path\to\mingw64\bin\mingw32-make.exe" `
  -DCMAKE_CXX_FLAGS="-D_WIN32_WINNT=0x0A00" `
  -DCMAKE_C_FLAGS="-D_WIN32_WINNT=0x0A00"

cmake --build build --parallel
```

* **`-G "MinGW Makefiles"` 是必要的。** Ninja generator 在 Windows 上可能永久卡住（本專案有記錄：
  `ninja` 會讀取子行程的輸出管道直到 EOF，而某些環境下該管道永遠不會關閉）。
* **`-D_WIN32_WINNT=0x0A00` 是必要的。** 內附的 `cpp-httplib` 在較低的 Windows 版本會拒絕編譯
  （`httplib.h: #error cpp-httplib doesn't support Windows 8 or lower`），而 MinGW 預設使用較舊的
  值。`build_fixit.ps1` 會傳入這個 define，`CMakeLists.txt` 目前尚未。

build 完成後，`build\bin\` 會有全部 8 個執行檔，而 macOS/Linux 的 tasks（`run:…`、`test:…`）在
Windows 上也能使用 —— 你的機器此時符合它們的預期。刪除 `build/` 之後必須重跑上述指令；
`cmake: configure` 不會帶上這兩個旗標。

---

## 7. 在編輯器中連接真實模型

`--llm mock` 只認得四種示範錯誤，其餘一律不處理。若要修復任意程式碼，必須指向一個模型。

### 本機模型 —— 不需要 key、不需要 relay

任何 OpenAI 相容、使用純 HTTP 的服務都可以；最快的是 [Ollama](https://ollama.com/download)。
（下文的 `fixit` 指你 build 出來的執行檔 —— macOS/Linux 是 `./build/bin/fixit`，Windows 是
`build-win\bin\fixit.exe`。）

```bash
ollama pull qwen2.5-coder:7b
```

```bash
fixit your_file.cpp --agent --llm openai \
  --base-url http://127.0.0.1:11434/v1 --model qwen2.5-coder:7b --api-key ollama --verbose --no-write
```

### 雲端端點 —— relay

預設 build 沒有 TLS，因此 `--base-url https://…` 會回報 *this build has no TLS support*。Relay
（`tools/windows/llm-relay.py`，純 Python、無依賴）以純 HTTP 對 FixIt、以 HTTPS 對你的端點，並讓 key
不會出現在 `fixit` 的命令列。

將 key 放入 git-ignored 檔案（或使用環境變數），再於第二個終端機啟動 relay：

```bash
# key
mkdir -p .secrets && printf '%s' 'sk-...' > .secrets/fixit_key      # macOS/Linux
# New-Item -ItemType Directory -Force .secrets; Read-Host | Set-Content -NoNewline .secrets\fixit_key   # Windows

# relay（保持開著）
RELAY_TARGET=https://your-endpoint/v1 python tools/windows/llm-relay.py
# PowerShell: $env:RELAY_TARGET = "https://your-endpoint/v1"; python tools/windows/llm-relay.py
```

然後在第一個終端機（或 task）執行 FixIt：

```bash
fixit your_file.cpp --agent --llm openai \
  --base-url http://127.0.0.1:8791/v1 --model your-model --api-key relay \
  --iterations 4 --verbose --no-write
```

`--api-key relay` 只是佔位符 —— 任何非空字串都可以；真正的憑證由 relay 注入。執行長時間任務之前先
確認端點：

```bash
python tools/windows/probe-endpoint.py "https://your-endpoint/v1" "your-model"
```

看到 `200` 與 `VERDICT : endpoint reachable, key accepted, model served, tools field accepted` 就代表
整條鏈路正常。兩個經驗法則：**4 輪**通常足夠（某一輪只呼叫 `read` 是正常的），以及**模型必須支援
tool calling** —— 若它只回傳文字，`tool_calls` 會一直為空，什麼都不會被 patch。

---

## 8. 疑難排解

| 症狀 | 原因 | 解法 |
|---|---|---|
| Task 閃一下後沒有反應 | `${file}` 為空 —— 沒有開啟檔案，或尚未儲存 | 在編輯器開啟一個 `.cpp` 並儲存 |
| `'cmake' 不是內部或外部命令` | 沒有安裝 CMake，或 `Ctrl+Shift+B` 在 Windows 選了預設 CMake task | 安裝 CMake，或改用 **Run Task… → `windows: build`** |
| `the compiler could not be executed` | `PATH` 上沒有 `g++` | 以 `--compiler` 傳入完整路徑，並將該 `bin` 資料夾加入 `PATH`（`as.exe`／`ld.exe` 在該處尋找） |
| `fixit: 'x.cpp' is not a readable file` | 相對路徑對應到錯誤的資料夾 | task 中使用 `${file}`，或使用絕對路徑 |
| `this build has no TLS support` | `--base-url https://…` | 使用 relay（第 7 節） |
| `RELAY_TARGET is not set.` | 啟動 relay 時沒有設定環境變數 | 設定後重新啟動 relay |
| `HTTP 401` / `404` | key 錯誤／端點或模型名稱錯誤 | 先執行 `probe-endpoint.py` |
| `cannot listen on 127.0.0.1:8791` | 已經有 relay 在執行 | 直接使用它，或傳入 `RELAY_PORT`／`-Port` |
| configure 時 `ninja` 卡住 | Windows 使用了 Ninja generator | `-G "MinGW Makefiles"`（第 6 節） |
| `httplib.h:30:2: error: #error` | `_WIN32_WINNT` 太低 | 加入 `-D_WIN32_WINNT=0x0A00`（第 6 節） |
| F5：*program '…/build/bin/fixit' does not exist* | 尚未做 CMake build，且 Windows 路徑少了 `.exe` | 用 CMake build（第 6 節），再使用第 5 節的 Windows 設定 |
| 輸出為亂碼（`鈺愨晲`） | 終端機不是 UTF-8 | `chcp 65001`（Windows），或使用 launcher（它會自動處理） |

離開代碼：`0` 乾淨或已修好、`1` 未修好、`2` 用法錯誤、`3` 內部錯誤。編譯器無法啟動時會顯示
`the compiler could not be executed`，exit code 為 `1`。

---

## 9. 值得安裝的 extension

| Extension | 原因 |
|---|---|
| `ms-vscode.cpptools` | F5（`cppdbg`）與本專案的 IntelliSense 都需要它 |
| `ms-vscode.cmake-tools` | 可選；提供狀態列與 `CMake: Configure`，但內建 tasks 完全不需要它 |

repository 的 `.vscode/settings.json` 已將 `compile_commands.json` 指向 `build/`，因此只要執行過一次
CMake configure，IntelliSense 就能解析內附的標頭（tree-sitter、Catch2、`httplib`）。
