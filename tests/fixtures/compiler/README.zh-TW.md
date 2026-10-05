> [English](README.md) | **繁體中文**

# 編譯器診斷測試資料

`fixit::parse_diagnostics()` 的黃金測試資料（請參見 `tests/compiler.test.cpp`）。
此測試套件是**離線且具確定性的**：它從不執行編譯器，只會重播下列的 `.txt`
擷取內容。`tools/record_compiler_fixtures.sh` 是唯一會與真實編譯器溝通的
東西。

## 目錄結構

```
<name>.json   metadata: dialect, compiler, source, provenance and the exact
              diagnostics parse_diagnostics() must produce ("expect")
<name>.txt    the raw bytes the compiler wrote to stderr (or stdout)
sources/      the small purpose-built translation units behind the new fixtures
```

測試會探索此目錄中的每個 `*.json`，載入 `<name>.txt`，並以
`dialect == "clang-json"` 選取 clang/JSON 進入點、以 `"gcc"` 選取文字進入點
來剖析它。

測試結構描述之外的額外中介資料欄位：

* `"recorded"` — `"machine"`（擷取自真實的編譯器呼叫）或
  `"synthetic"`（手工撰寫；請參見下方的*來源*）。
* `"output_format"` — `"text"` 或 `"json"`，也就是磁碟上實際的序列化格式。

## 這 20 個測試資料

| 測試資料 | 方言 | 編譯器 | 來源 | 輸出 | 記錄方式 | 涵蓋 |
| --- | --- | --- | --- | --- | --- | --- |
| `e1_missing_include.gcc` | gcc | gcc | `examples/buggy/e1_missing_include.cpp` | text | synthetic | 對等組，缺少 `;`，多重錯誤連鎖 |
| `e2_drift.gcc` | gcc | gcc | `examples/buggy/e2_drift.cpp` | text | synthetic | 對等組，錯誤遠離開頭 |
| `e3_type_error.gcc` | gcc | gcc | `examples/buggy/e3_type_error.cpp` | text | synthetic | 對等組，`note:` 行會被捨棄 |
| `n01_multi_error.clang` | clang-json | clang | `multi_error.cpp` | text | machine | 同一個檔案中有兩個錯誤 |
| `n02_warning_only.clang` | clang-json | clang | `warning_only.cpp` | text | machine | 只有警告的輸出 |
| `n03_fatal_error.clang` | clang-json | clang | `missing_header.cpp` | text | machine | `fatal error:` |
| `n04_clean.clang` | clang-json | clang | `clean.cpp` | text | machine | 完全沒有診斷（0 位元組的擷取內容） |
| `n05_dir_prefix.clang` | clang-json | clang | `src/foo.cpp` | text | machine | 帶有目錄前置字元的檔案路徑 |
| `n06_note_attached.clang` | clang-json | clang | `note_attached.cpp` | json | synthetic | 子 `note` 併入錯誤訊息中 |
| `n07_context_line.clang` | clang-json | clang | `context_line.cpp` | json | synthetic | `source` 陣列重建為 `context_line` |
| `n08_json_multi_error.clang` | clang-json | clang | `multi_error.cpp` | json | synthetic | 走訪 JSON 陣列；最上層的 `note` 被捨棄 |
| `n09_json_clean.clang` | clang-json | clang | `clean.cpp` | json | synthetic | 空的 JSON 陣列 |
| `n10_json_warning.clang` | clang-json | clang | `warning_only.cpp` | json | synthetic | `warning` 層級 + 併入的 note |
| `n11_json_fatal_error.clang` | clang-json | clang | `missing_header.cpp` | json | synthetic | `fatal error` 層級 |
| `n12_warning_only.gcc` | gcc | gcc | `warning_only.cpp` | text | synthetic | 只有警告的輸出 |
| `n13_caret_context.gcc` | gcc | gcc | `caret_context.cpp` | text | synthetic | 插入號區塊 → `context_line` |
| `n14_col1.gcc` | gcc | gcc | `col1_error.cpp` | text | synthetic | 位於第 1 欄的診斷 |
| `n15_long_message.gcc` | gcc | gcc | `long_message.cpp` | text | synthetic | 約 250 個字元的訊息 |
| `n16_dir_prefix.gcc` | gcc | gcc | `src/foo.cpp` | text | synthetic | 帶有目錄前置字元的檔案路徑 |
| `n17_fatal_error.gcc` | gcc | gcc | `missing_header.cpp` | text | synthetic | `fatal error:` + 結尾的 `compilation terminated.` |

九個測試資料是 `gcc`，十一個是 `clang-json`。

此外，此目錄保留了三個**沒有 `.json` 的參考擷取內容**——
`e1_missing_include.clang.txt`、`e2_drift.clang.txt`、`e3_type_error.clang.txt`。
它們是三個對等組中已簽入、由機器記錄的 clang 部分；
測試會斷言每個 `.clang.txt` 的 `(line, column)` 集合與錯誤數量
都與其手工撰寫的 `.gcc.txt` 對應檔相符。它們刻意不屬於
那 20 個以中介資料為基礎的測試資料。

## 來源——哪些是機器記錄的，哪些是手工撰寫的

用來產生這些測試資料的機器是 macOS，搭載 **Apple clang 17** 且
**沒有真正的 GCC**：

* **`recorded: machine`**——由
  `tools/record_compiler_fixtures.sh` 從真實的 `clang++` 記錄。Apple clang 17
  會拒絕 `-fdiagnostics-format=json`（只存在 `clang`、`msvc` 和 `vi`），因此
  指令碼會退回 clang 的文字格式，而這些擷取內容便是文字。
  `fixit::parse_diagnostics()` 對於非預期的 clang 組建，其處理方式完全符合
  預期：它找不到 JSON 內容，於是退回文字剖析器，因此 `clang-json` 進入點
  仍然有被實際演練到。
* **`recorded: synthetic`、GCC 方言**——這裡沒有安裝真正的 GCC，而且這台機器
  上的 `g++` 是指向 clang 的符號連結，所以不可能有機器記錄的 `.gcc.txt`。
  每個 GCC 測試資料都是依照 GCC 文件所述的輸出樣式（`file:line:col: level:
  message`、一行 `%5d | src` 邊欄行，以及一行插入號行）手工撰寫。
  在裝有真正 GCC 的機器上，`tools/record_compiler_fixtures.sh` 會覆寫它們，
  而 `expect` 區塊必須對照真實輸出重新審查。
  而 `expect` 區塊必須對照真實輸出重新審查；
* **`recorded: synthetic`、clang JSON 方言**——那六個 `n06`…`n11` 測試資料
  以 clang 文件所述的 `-fdiagnostics-format=json` 物件結構描述來序列化
  *實際上*由相同來源的 clang 文字擷取內容所產生的診斷（相同訊息文字、
  相同檔案/行/欄、相同 note）。只有序列化部分是手工撰寫的，因為這裡沒有
  clang 能夠產生它。它們的 `sources/*.cpp` 檔案就是真實文字擷取內容的來源，
  因此兩者可以並排比較。
  因此兩者可以並排比較。
* `sources/` 底下的一切都是為此測試套件專門打造的。

### 對等組在構造上就是對齊的

GCC 的錯誤復原方式與 clang 不同：對於 `e1`/`e2`，真正的 GCC 會比 clang 更早
停止連鎖，而對於 `e3`，GCC 會把轉換錯誤錨定在引數上，而不是錨定在呼叫上。
因此，手工撰寫的 `.gcc.txt` 檔案帶有與已簽入的 clang 擷取內容相同的
`(line, column)` 錨點，這正是讓跨方言的對等性斷言具有意義且穩定的原因。
訊息*措辭*是 GCC 自己的（例如 `'vector' in namespace 'std' does
not name a template type`）；連鎖位置則是刻意設計成與 clang 共用。
在裝有真正 GCC 的機器上重新記錄會改變它們，而對等性測試
正是會指出這點的機制。
而對等性測試正是會指出這點的機制。

## 重新產生

```sh
tools/record_compiler_fixtures.sh
```

此指令碼是幂等的，而且從不刪除任何東西。除非它能明確辨識出真正的 GCC
（它會用 `g++ --version` / `-dumpversion` 來 grep `clang`），否則它會跳過每個
`.gcc.txt`，而且只有在第二道防護拒絕了看起來像 clang 的內容之後，才會寫入
`.gcc.txt`。當支援 `-fdiagnostics-format=json` 時，它會用該選項記錄 clang，
否則就用 clang 文字格式，而且它不會去動合成（synthetic）的 clang JSON
測試資料。

當擷取內容變更時，請審查對應的 `.json` `expect` 區塊：指令碼只會更新原始
輸出，並不會重新推導期望值。


## 對等性：行可以，欄不行

那三個 `examples/buggy` 來源會由兩種方言編譯，並以兩種方式
比較：

* `compiler.test` 比較**擷取內容**（`.gcc.txt` 對 `.clang.txt`）。
* 第二個案例比較來自真實 `g++` 與真實 `clang++` 的**即時輸出**，
  當其中任一不存在時便會跳過自己。

兩者都會精確比較錯誤**行**集合，並且刻意不比較欄。
真實編譯器對於診斷指向哪個權杖並不一致。最明顯的
案例是 `e3_type_error.cpp` 的 `count_words(value)`：

| 編譯器 | 定位依據 | 原因 |
| --- | --- | --- |
| clang | `13:15` | 被以不良引數呼叫的識別項 |
| GCC | `13:25` | 無法轉換的引數運算式 |

手工撰寫的 GCC 測試資料使用 clang 的欄，好讓合成語料庫保持內部一致；
在 GCC 機器上重新記錄它們會改變那些欄，而兩個對等性案例的撰寫方式都預期
這種情況。修復迴圈需要的是行——那才是選取區塊脈絡的依據——而兩個編譯器
在全部三個來源的行上都是一致的。
在全部三個來源的行上都是一致的。
