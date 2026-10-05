> [English](wiki_outline.md) | **繁體中文**

# Wiki 大綱（W3）

這是專案 Wiki 的單頁結構。每一節說明該處要放什麼，以及本儲存庫中的
哪一份產出物提供該內容 —— 所有東西都已經由工具產生好了，所以 Wiki 只是
貼上的問題，而不是重新量測的問題。

---

## 1. 動機 —— 為什麼「patch 沒有套用成功」才是瓶頸

**先講失敗，再講功能。** 模型寫出正確的編輯，`@@` 標頭卻慢了兩行，
`patch(1)` 就拒絕整份 diff，而模型永遠不會知道原因。三個具體產出物：

* 來自 README §6.3 的 `PatchEngine::failure_summary()` 範例 —— 模型
  實際收到的文字；
* `docs/demo_output.txt` 的 iteration 2，其中一個 hunk 宣告 `L11` 卻落在
  `L10`（`fuzzy, drift-1`），而修復仍然完成；
* 誠實的反例：`docs/patch_success_heatmap_ambiguous.svg` 中的
  identical-block 欄位，引擎在此*無法*做出選擇。

## 2. 示範 —— GIF / 影片

用 `tools/record_demo.sh` 現場錄製（指令稿會寫出文字
逐字稿；GIF 則擷取終端機畫面）。60–90 秒：

1. `fixit e1_missing_include.cpp --agent --llm mock --verbose` → exit 0。
2. `fixit e2_drift.cpp --agent --llm mock --verbose` → 在 37 行漂移之下仍 exit 0。
3. `fixit e3_type_error.cpp --agent --llm mock --verbose` → exit 1，並誠實回報。
4. `ctest --test-dir build --output-on-failure` → `100% tests passed`。

把 GIF 放在頁面最上方。同一份逐字稿的文字版本是
`docs/demo_output.txt`，由同一支指令稿重新產生。

## 3. 有效性 —— patch 成功率矩陣

**圖 1（主要結果）：** `docs/patch_success_heatmap.svg`
4 種 impairment × 7 種漂移距離，每格 200 次試驗，格式良好的 diff。
平均套用率 100%，最差的一格 99%。

**圖 2（壓力測試）：** `docs/patch_success_heatmap_shared_context.svg`
會重複出現的樣板碼（`}`、空行、幾乎相同的函式主體）中只有
一行獨特的行需要修復：在 ±20 行漂移之內仍然 100%。

**圖 3（極限）：** `docs/patch_success_heatmap_ambiguous.svg`
每個區塊都位元組完全相同。在 ±2 行漂移以下它是 100%；到了 ±5 及
更遠則是 0%，因為宣告的位置是*唯一*可能用來消歧的資訊，而它是錯的。
這就是這項技術誠實的界線。

數字與方法：`docs/patch_success_matrix.json`，由
`./build/bin/fixit-matrix` 產生。用以下指令重新產生兩者：

```bash
./build/bin/fixit-matrix docs/patch_success_matrix.json
python3 tools/render_heatmap.py            # writes the three SVGs
```

熱圖是純 SVG（沒有繪圖相依性），因此它們能在 Wiki 中顯示，
在 git 中也能好好地 diff。

## 4. 軌跡 —— 迴圈實際上做了什麼

截圖或貼上 `examples/buggy/expected/e1_missing_include.trajectory.json`：

```json
{
  "compiler": "clang++", "exit_code": 0, "success": true, "iterations": 2,
  "initial_errors": 5, "final_errors": 0,
  "rounds": [
    {"round": 1, "tool_calls": ["patch"], "errors_before": 5, "errors_after": 1},
    {"round": 2, "tool_calls": ["patch"], "errors_before": 1, "errors_after": 0}
  ]
}
```

`errors_before` 是模型被顯示的內容；`errors_after` 是編譯器
在它採取行動後所說的內容。完整追蹤（每輪的 `compile_before`、`compile_after`、
工具呼叫與觀察結果）就是 `--trace` 所寫出的東西。

## 5. 架構與程式碼指引

沿用 README 的 ASCII 圖（§5），並連結這五個標頭檔：
`include/fixit/{compiler,codemap,patch,agent}.h`。特別指出讓這個迴圈
值得信賴的兩條設計規則：

* 編譯器是唯一的事實依據（即使是 `FINAL` 答案仍會觸發
  重新編譯）；
* 每一次失敗都是結構化的（`score`、gate、最接近的位置、違規的行、
  建議重新讀取），而不是一個結束碼。

## 6. 各值得一段的工程筆記

* 在我們的程式碼上用 `-Wall -Wextra -Werror`，絕不用於第三方程式碼（ADR-014）。
* 方言探測：Apple clang 17 拒絕兩個 JSON 旗標，因此剖析器
  降級為文字，而不是失去診斷資訊（ADR-007）。
* 讓 `gate = 0.8` 得以達到的評分正規化（ADR-002）。
* 以 0 為基準的位置慣例，消滅了一個漂移 hunk 重寫的 bug（ADR-001）。

## 7. 連結

* 儲存庫與 README 快速開始。
* `docs/decisions.md` —— 全部 29 條 ADR（ADR-001 … ADR-029）。
* CI 徽章（Ubuntu 上的 gcc-12、macOS 上的 clang）—— 兩個平台都會把關合併。

---

## 重新產生每一項 Wiki 素材

```bash
# 1. build
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j

# 2. demo transcript + corrected colours
tools/record_demo.sh

# 3. success-rate matrix and the three heat maps
./build/bin/fixit-matrix docs/patch_success_matrix.json
python3 tools/render_heatmap.py

# 4. expected trajectories (per-example assertions)
tools/gen_expected.sh

# 5. the full evidence trail
ctest --test-dir build --output-on-failure
```
