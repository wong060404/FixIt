> [English](WINDOWS-SETUP.md) | **繁體中文**

# FixIt Windows 完整指南（複製貼上就能用）

> 從 `git clone` 到修好第一個 bug，每一步都寫清楚「打什麼」和「應該看到什麼」。
> 不需要任何 C++、編譯器或 Git 的基礎知識。
>
> 全部指令都在繁體中文 Windows 11 + PowerShell 上實測過。

---

## 0. 這份指南會帶你到哪裡

| 階段 | 你會做到 | 大概時間 |
|---|---|---|
| 安裝（一次就好） | 下載程式、下載編譯器、編譯出 `fixit.exe` | 10–25 分鐘 |
| 使用 | 用 `fixit` 找出並修好 C++ 的錯誤 | 每次幾秒到幾分鐘 |
| （選用）接 AI 模型 | 用你自己的 API 或本機模型自動修任意 bug | 5 分鐘 |

裝完之後，你電腦上會多出這些東西：

```
C:\FixIt\                          ← 工作資料夾（純英文路徑，重要！）
├── FixIt\                         ← 從 GitHub 複製下來的專案
│   ├── build-win\bin\fixit.exe    ← 編譯出來的工具（主角）
│   └── tools\windows\             ← 本指南用到的輔助腳本
└── toolchain\                     ← 編譯器（解壓即用，免安裝）
```

---

## 開始前：三個必須知道的規則

**規則 1：整個路徑不能有中文、空白或特殊符號。**
請用 `C:\FixIt`。原因是 Windows 上的 GCC 會把自己函式庫的**絕對路徑**交給連結器，
而連結器用本地編碼解讀；路徑含中文就會變成亂碼，然後出現一堆看不懂的
`No such file or directory`。這一步做錯，後面會卡得很痛苦。

**規則 2：不要放在 OneDrive、桌面或「文件」資料夾裡。**
編譯器和建置產物加起來約 2 GB，OneDrive 會把它們同步上雲端，拖慢整台電腦；
而且它的「檔案隨選」機制可能讓執行檔變成雲端佔位檔而無法執行。

**規則 3：FixIt 需要一個真正的 C++ 編譯器。**
因為它的工作就是「編譯 → 看錯誤 → 修 → 再編譯」。指南第 3 步會幫你下載一個
免安裝的 GCC，不需要 Visual Studio。

---

## 第一部分：安裝（做一次就好）

### 步驟 1：確認 Git 與 Python

按 **Win 鍵**，輸入 `powershell`，按 **Enter**，會出現一個文字視窗。之後所有指令都打在這裡。

```powershell
git --version
python --version
```

**應該看到**兩行版本號，例如：

```
git version 2.45.1.windows.1
Python 3.12.10
```

如果有任何一個顯示「無法辨識」：

* **Git** → 到 <https://git-scm.com/download/win> 下載，一路按 Next 安裝完。
* **Python** → 到 <https://www.python.org/downloads/windows/> 下載，
  **安裝時務必勾選最下方的 `Add python.exe to PATH`**。

裝完**關掉 PowerShell 再重開**，再確認一次。

### 步驟 2：建立工作資料夾並下載 FixIt

```powershell
New-Item -ItemType Directory -Force -Path C:\FixIt | Out-Null
Set-Location C:\FixIt
git clone https://github.com/wong060404/FixIt.git
```

**應該看到**：

```
Cloning into 'FixIt'...
Resolving deltas: 100% (.../...), done.
```

驗證：

```powershell
Test-Path C:\FixIt\FixIt\CMakeLists.txt
```

**應該看到** `True`。

> 沒有 Git 的話：在 GitHub 網頁按綠色 **Code** → **Download ZIP**，解壓縮後把資料夾
> 改名為 `FixIt`，放到 `C:\FixIt\` 底下。
>
> **Clone 之後不需要再套用任何補丁或複製任何檔案**——專案本身已經內含 Windows 支援
> 與這份指南用到的輔助腳本。

### 步驟 3：下載編譯器（約 274 MB）

```powershell
python C:\FixIt\FixIt\tools\windows\fetch_mingw.py
```

這會從 GitHub 下載 winlibs 的 MinGW-w64 GCC，解壓到 `C:\FixIt\toolchain\`（解壓後約 1.5 GB）。

**應該看到**（最後幾行）：

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

驗證：

```powershell
& C:\FixIt\toolchain\mingw\mingw64\bin\g++.exe --version
```

**應該看到**第一行類似：

```
g++.exe (MinGW-W64 x86_64-ucrt-posix-seh, built by Brecht Sanders, r2) 16.2.0
```

> 下載完可以把 `C:\FixIt\FixIt\tools\windows\.cache\` 裡的 zip 刪掉，省 274 MB。
> 之後要重建也很簡單，重跑一次這個腳本就好。

### 步驟 4：編譯 FixIt（約 1–3 分鐘）

```powershell
powershell -ExecutionPolicy Bypass -File C:\FixIt\FixIt\tools\windows\build_fixit.ps1
```

（`-ExecutionPolicy Bypass` 只是允許執行這個腳本，不會改變系統設定。）

**應該看到**一連串 `CC ...`、`CXX ...`，最後是：

```
AR  build-win/libfixit.a
CXX tools/fixit-cli/main.cpp
built: C:\FixIt\FixIt\build-win\bin\fixit.exe
CXX tools/matrix.cpp
built: C:\FixIt\FixIt\build-win\bin\fixit-matrix.exe
```

驗證：

```powershell
Test-Path C:\FixIt\FixIt\build-win\bin\fixit.exe
```

**應該看到** `True`。

### 步驟 5：立刻試跑（不用網路、不用帳號）

```powershell
chcp 65001 > $null
$env:PATH = "C:\FixIt\toolchain\mingw\mingw64\bin;$env:PATH"
$env:NO_COLOR = "1"
Set-Location C:\FixIt\FixIt
.\build-win\bin\fixit.exe examples\buggy\e1_missing_include.cpp --agent --llm mock --verbose --no-write --no-timing
```

**應該看到**（最後幾行）：

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

**看到 `✔ Fixed ...` 就代表安裝完全成功。** 你剛剛跑的是一個完整的自動修復循環：

| 輸出 | 意義 |
|---|---|
| `✗ 1 error` / `[E1] L12 …` | FixIt 呼叫真正的編譯器，把錯誤變成結構化資料 |
| `→ context: fn parse_count() [L10–L13]` | 用 tree-sitter 找出這行錯誤落在哪個函式 |
| `→ patch…` → `✓ hunk 1 @ L11` | 修補被套用；`fuzzy, drift-1` 表示行號差了一行仍被容錯機制救回 |
| `✓ clean` | 重新編譯驗證，只有編譯器能宣告成功 |
| `✔ Fixed 10 errors in 2 iterations` | 結果（exit code 0） |

> `--no-write` 代表只改暫存副本、不動原始檔。想真的改檔案就拿掉這個旗標。
> `--llm mock` 是內建的離線假模型，只認得四種示範用錯誤——要修你自己的任何 bug，看第二部分。

---

## 第二部分：日常使用

每個新開的 PowerShell 視窗，先打這三行（或見附錄 B 設成永久）：

```powershell
chcp 65001 > $null
$env:PATH = "C:\FixIt\toolchain\mingw\mingw64\bin;$env:PATH"
$env:NO_COLOR = "1"
```

| 那三行在幹嘛 | 說明 |
|---|---|
| `chcp 65001` | 把主控台切成 UTF-8，否則 FixIt 的框線字元會顯示成 `鈺愨晲` 這種亂碼（只是顯示問題，不影響功能） |
| 設 `PATH` | 讓 fixit 找到 `g++`。不設也可以，但要每次加 `--compiler "C:\FixIt\toolchain\mingw\mingw64\bin\g++.exe"` |
| `NO_COLOR` | 關掉顏色，輸出比較好複製 |

### 用法 A：只想找出錯在哪（最快，不用模型）

```powershell
C:\FixIt\FixIt\build-win\bin\fixit.exe "C:\你的專案\你的檔案.cpp"
```

**應該看到**每個錯誤的行號、欄位與訊息。看完就知道要改哪裡。

### 用法 B：看程式結構

```powershell
C:\FixIt\FixIt\build-win\bin\fixit.exe "C:\你的專案\你的檔案.cpp" --outline
```

會列出：有幾個函式、各佔哪幾行、include 了什麼、哪一行有語法錯誤。適合快速熟悉一份陌生程式碼。

### 用法 C：接上 AI 模型，自動修復

FixIt 支援任何 **OpenAI 相容**的端點。分兩種情況，選一種就好：

#### C-1. 本機模型（最簡單，不用金鑰、不用轉發器）

先裝 [Ollama](https://ollama.com/download)，然後下載一個支援工具呼叫的模型：

```powershell
ollama pull qwen2.5-coder:7b
```

直接指過去即可（Ollama 走純 http，不需要轉發器）：

```powershell
C:\FixIt\FixIt\build-win\bin\fixit.exe "C:\你的專案\你的檔案.cpp" `
  --agent --llm openai `
  --base-url http://127.0.0.1:11434/v1 --model qwen2.5-coder:7b --api-key ollama `
  --verbose --no-write
```

#### C-2. 你自己的雲端 API（https，需要一個本機轉發器）

多一層轉發器的原因只有一個：**這個建置沒有 TLS**，fixit 自己不能連 `https://`。
轉發器讓 fixit 走純 http 連本機，由它帶著你的金鑰走 https 到你的端點。
附帶好處是**金鑰不會出現在 fixit 的命令列**。

**先放金鑰**（這個位置已被 `.gitignore` 排除，不會被 commit）：

```powershell
New-Item -ItemType Directory -Force -Path C:\FixIt\FixIt\.secrets | Out-Null
Read-Host -Prompt "貼上 API key 後按 Enter" | Set-Content -NoNewline C:\FixIt\FixIt\.secrets\fixit_key
```

**先確認端點、金鑰、模型都可用**（幾秒鐘，避免後面白忙）：

```powershell
python C:\FixIt\FixIt\tools\windows\probe-endpoint.py "https://你的端點/v1" "你的模型名稱"
```

**應該看到**：

```
HTTP     : 200
VERDICT  : endpoint reachable, key accepted, model served, tools field accepted
```

`HTTP : 401` 是金鑰不對、`404` 是端點或模型名稱打錯、`transport error` 是網路不通。

**再開兩個視窗：**

視窗 A（轉發器，保持開著不要關）：

```powershell
$env:RELAY_TARGET = "https://你的端點/v1"
python C:\FixIt\FixIt\tools\windows\llm-relay.py
```

**應該看到**：

```
relay      : http://127.0.0.1:8791  ->  https://你的端點/v1
key loaded : yes
```

> **轉發器開著的時候，你的金鑰會留在它的記憶體裡**，所以用完按 **Ctrl+C** 關掉比較安心。
> 如果它說 `cannot listen on 127.0.0.1:8791`，代表你已經有另一個轉發器在跑；
> 直接用它就好，或先關掉舊的再啟動。

視窗 B（讓 FixIt 修你的檔案）：

```powershell
chcp 65001 > $null
$env:PATH = "C:\FixIt\toolchain\mingw\mingw64\bin;$env:PATH"
$env:NO_COLOR = "1"

C:\FixIt\FixIt\build-win\bin\fixit.exe "C:\你的專案\你的檔案.cpp" `
  --agent --llm openai `
  --base-url http://127.0.0.1:8791/v1 --model "你的模型名稱" --api-key relay `
  --verbose --no-write
```

**強烈建議先加 `--no-write`**：只改暫存副本，你可以先看它打算怎麼改，確認滿意後
把 `--no-write` 拿掉再跑一次，它就會直接改你的檔案。

**應該看到**類似（以一個大小寫打錯的檔案為例）：

```
── iteration 1 ──  ✗ 1 error  [E1] L6 'S' was not declared in this scope
                  → context: fn main() [L4–L8]
── iteration 2 ──  → patch…  ✓ hunk 1 @ L6  (exact)
                  ✓ clean
✔ Fixed 1 errors in 2 iterations (3.5s)
```

模型可能先呼叫 `read` 多看幾行程式，所以某一輪沒有 `patch` 是正常的。

#### C-3. 一鍵跑完整循環（選用）

想看到逐輪紀錄、metrics 與修改前後的 diff，用這支腳本（它會自己起轉發器、跑完再收掉）：

```powershell
powershell -ExecutionPolicy Bypass `
  -File C:\FixIt\FixIt\tools\windows\run-real-loop.ps1 `
  -BaseUrl "https://你的端點/v1" -Model "你的模型名稱" -File "C:\你的專案\你的檔案.cpp"
```

不給 `-File` 的話，它會自己在 `%TEMP%\fixit-demo\demo_bug.cpp` 造一個有一處錯誤的小檔案
並修給你看，完全不用準備任何東西。

### 常用旗標速查

| 旗標 | 作用 |
|---|---|
| `--agent` | 啟動修復迴圈；不給就只編譯一次並列出診斷 |
| `--llm mock` / `--llm openai` | 模型後端；預設 `mock` |
| `--base-url` / `--model` / `--api-key` | 端點、模型、金鑰（金鑰也可用環境變數 `FIXIT_API_KEY`） |
| `--no-write` | 只修暫存副本，原檔不動（強烈建議先用這個看結果） |
| `--verbose` | 逐輪顯示診斷、定位、修補命中位置 |
| `--trace PATH` / `--metrics PATH` | 寫出 JSON 軌跡／統計（每輪模型呼叫了什麼、修補成功率） |
| `--iterations N` | 最多幾輪修復（預設 4） |
| `--compiler NAME` | 指定編譯器（預設 `g++`） |
| `-I DIR` / `-D NAME[=VALUE]` / `--flag FLAG` | 原樣傳給編譯器，可重複（檔案 include 專案標頭時會用到） |
| `--outline` | 只印結構，不編譯 |
| `-h` / `--help` | 用法 |

離開代碼：`0` 乾淨或已修好、`1` 未修好、`2` 用法錯誤、`3` 內部錯誤、
`127` 找不到編譯器（PATH 或 `--compiler` 沒設好）。

---

## 疑難排解

| 你看到的訊息 | 原因 | 解法 |
|---|---|---|
| `'git' 不是內部或外部命令` | 沒裝 Git | 步驟 1 |
| `'python' 不是內部或外部命令` | 沒裝 Python，或安裝時沒勾 Add to PATH | 步驟 1，裝完要重開視窗 |
| `fetch_mingw.py` 說 destination contains non-ASCII | 工作路徑含中文 | 改用 `C:\FixIt`，重新 clone |
| 編譯時一堆 `No such file or directory`、`cannot find -lstdc++` | 編譯器被放在中文路徑下 | 把整個 `C:\FixIt` 換成純英文路徑 |
| `compiler not found. Looked in: ...` | 還沒下載編譯器 | 先做步驟 3 |
| `the compiler could not be executed (exit code 127)` | `g++` 不在 PATH，也沒用 `--compiler` 指定 | 跑第二部分開頭那三行 |
| `this build has no TLS support; rebuild with -DFIXIT_ENABLE_OPENSSL=ON` | `--base-url` 直接指到 `https://` | 用 C-2 的轉發器，或改用 C-1 的本機模型 |
| `RELAY_TARGET is not set.` | 沒告訴轉發器要連哪裡 | 先 `$env:RELAY_TARGET = "https://你的端點/v1"` |
| `cannot listen on 127.0.0.1:8791` / `port 8791 is already serving a relay for a different endpoint` | 已經有一個轉發器在跑 | 直接用那個，或先關掉舊的，或 `-Port 8792` 換一個埠 |
| `LLM request returned HTTP 401` / `404` | 金鑰錯 / 端點或模型名稱錯 | 用 `probe-endpoint.py` 先確認 |
| 轉發器顯示 `key loaded : no` | 金鑰檔不在 `C:\FixIt\FixIt\.secrets\fixit_key` | 見 C-2 |
| `tool_calls` 一直是空的、模型只回文字 | 該模型不支援工具呼叫 | 換一個支援 function calling 的模型 |
| 輸出變成 `鈺愨晲`、`鉁?` 這類亂碼 | 主控台不是 UTF-8 | 執行 `chcp 65001` |
| 執行 `.ps1` 說「因為這個系統上已停用指令碼執行」 | PowerShell 預設禁止 | 如步驟 4 加上 `-ExecutionPolicy Bypass` |
| 關掉視窗後 PATH 又失效 | 環境變數只在該視窗有效 | 見附錄 B |

---

## 附錄 A：為什麼要這樣做（原理）

**為什麼要另外下載一個編譯器？**
FixIt 的用途就是驅動一個真正的編譯器並解讀它的輸出，所以它自己需要一個 C++ 編譯器：
一是拿來編譯 FixIt 本身，二是拿來當 FixIt 要驅動的那個編譯器。這裡用的是 winlibs 的
MinGW-w64 GCC——免安裝、免管理員權限、解壓就能用，而且它的診斷格式是專案明確支援的。

**為什麼路徑一定要 ASCII？**
gcc 會把自己安裝目錄下的函式庫路徑交給連結器 `ld`。路徑含中文時，這串位元組會被用本地
代碼頁（例如 936/GBK）重新解讀，於是 `crt2.o`、`-lstdc++` 全部找不到，
錯誤訊息還完全指不到真正的原因。放在 `C:\FixIt\toolchain` 就完全避開。

**為什麼 Windows 上的建置腳本不用專案的 `cmake`？**
在這個環境試過，有兩個獨立問題：`ninja` 會永久卡住（它會讀子行程的輸出管道直到 EOF，
而某些環境下那個管道不會關閉）；`mingw32-make` 會把 CMake 產生的 UTF-8 Makefile 當成
本地編碼讀，遇到中文路徑就變成亂碼而找不到檔案。`build_fixit.ps1` 做的是完全相同的事
（C 用 gcc、C++ 用 g++、專案程式碼開 `-Wall -Wextra -Werror`），只是不經過 CMake。
Linux / macOS 使用者照專案 README 的 `cmake` 流程即可。

**為什麼要一個本機轉發器？**
專案刻意不依賴 OpenSSL（決策 ADR-004），所以預設建置沒有 TLS 能力。
轉發器用 Python 內建的憑證處理走 https，FixIt 端只說純 http。
若你希望 FixIt 自己連 https，需要做一個帶 OpenSSL 的建置
（`-DFIXIT_ENABLE_OPENSSL=ON`），並且在 Windows 上用 `FIXIT_CA_BUNDLE` 指定 CA 憑證包。

**為什麼要用 `--no-write`？**
因為 FixIt 預設會直接改你的檔案。先看它打算怎麼改、確認沒問題再套用，比較安全。

## 附錄 B：把 PATH 設成永久（可選）

不想每個新視窗都打那三行：

```powershell
[Environment]::SetEnvironmentVariable(
  "Path",
  "C:\FixIt\toolchain\mingw\mingw64\bin;" + [Environment]::GetEnvironmentVariable("Path","User"),
  "User")
[Environment]::SetEnvironmentVariable("FIXIT_TOOLCHAIN", "C:\FixIt\toolchain", "User")
```

**要開新的 PowerShell 視窗才會生效。**（只改你這個使用者的設定，不動系統設定。）

## 附錄 C：全部指令速查

```powershell
# ── 安裝（一次就好）─────────────────────────────
New-Item -ItemType Directory -Force -Path C:\FixIt | Out-Null
Set-Location C:\FixIt
git clone https://github.com/wong060404/FixIt.git
python C:\FixIt\FixIt\tools\windows\fetch_mingw.py
powershell -ExecutionPolicy Bypass -File C:\FixIt\FixIt\tools\windows\build_fixit.ps1

# ── 每個新視窗 ──────────────────────────────────
chcp 65001 > $null
$env:PATH = "C:\FixIt\toolchain\mingw\mingw64\bin;$env:PATH"
$env:NO_COLOR = "1"

# ── 找錯在哪 ────────────────────────────────────
C:\FixIt\FixIt\build-win\bin\fixit.exe "C:\path\to\file.cpp"
C:\FixIt\FixIt\build-win\bin\fixit.exe "C:\path\to\file.cpp" --outline

# ── 離線示範（只認四種錯誤）─────────────────────
C:\FixIt\FixIt\build-win\bin\fixit.exe "C:\path\to\file.cpp" --agent --llm mock --verbose --no-write

# ── 用本機模型修（Ollama，不用金鑰）─────────────
C:\FixIt\FixIt\build-win\bin\fixit.exe "C:\path\to\file.cpp" --agent --llm openai `
  --base-url http://127.0.0.1:11434/v1 --model qwen2.5-coder:7b --api-key ollama --verbose

# ── 用雲端 API 修（需先開轉發器）────────────────
python C:\FixIt\FixIt\tools\windows\probe-endpoint.py "https://你的端點/v1" "模型"
$env:RELAY_TARGET = "https://你的端點/v1"
python C:\FixIt\FixIt\tools\windows\llm-relay.py          # 視窗 A，保持開著
C:\FixIt\FixIt\build-win\bin\fixit.exe "C:\path\to\file.cpp" --agent --llm openai `
  --base-url http://127.0.0.1:8791/v1 --model "模型" --api-key relay --verbose --no-write
```
