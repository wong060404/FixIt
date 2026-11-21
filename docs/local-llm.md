# Running FixIt against a local LLM

FixIt talks to any OpenAI-compatible endpoint. A local model needs no API key,
no relay and no TLS, which makes it the simplest way to repair arbitrary files
(the built-in `--llm mock` only recognises the four patterns in
`examples/buggy/`).

## 1. Quick start (Ollama)

```bash
# once
ollama pull qwen2.5-coder:7b        # any tool-calling model works

# repair a file from anywhere on disk
fixit "C:/Users/me/Downloads/broken.cpp" \
      --agent --llm openai \
      --base-url http://127.0.0.1:11434/v1 \
      --model qwen2.5-coder:7b \
      --api-key ollama \
      --verbose --no-write
```

`--api-key ollama` is a placeholder: Ollama ignores it, but `--llm openai`
requires the flag to be present.

Drop `--no-write` when the candidate patch is good and the file should actually
be repaired.

## 2. How much machine do you need?

Model size is the dominant factor. Rough guidance for a quantised
(`Q4_K_M`) model running on **one GPU**:

| Model size | VRAM needed | Typical hardware | Notes |
|---|---|---|---|
| 7B | ~5 GB | Any 8 GB GPU | Fast; good enough for the demo patterns |
| 14B | ~9 GB | 12 GB GPU | Noticeably better at multi-file context |
| 20-24B | ~14 GB | 16 GB GPU | Best quality/speed balance for this task |
| 32B | ~20 GB | 24 GB GPU | Slower; useful when context is large |
| 70B | ~40 GB+ | 48 GB GPU or multi-GPU | Usually not worth it for this workload |

**CPU-only inference works but is slow** (minutes per repair round). If you
only have integrated graphics, prefer a 7B model and `--iterations 2`.

To check what a specific machine can run, see <https://canirun.ai/> and enter
the CPU/GPU/RAM; it maps the hardware to model sizes.

## 3. Documenting a test

When a team member runs a local model, record it so the claim is checkable:

| Field | Example |
|---|---|
| Model | `qwen2.5-coder:7b` (Q4_K_M) |
| Hardware | RTX 4060 8 GB / Ryzen 7 / 32 GB RAM |
| Backend | Ollama 0.x, `http://127.0.0.1:11434/v1` |
| File | `e1_missing_include.cpp` |
| Result | fixed, 2 iterations, exit 0 |
| Notes | first round ~8 s on GPU |

Add the row to `docs/decisions.md` (ADR-004 covers endpoint support) or to this
file, and keep the command used so the run can be repeated.

## 4. Why tool calling matters

The repair loop drives the model through three tools (`compile`, `read`,
`patch`). A model that does not emit valid tool calls will loop until
`--iterations` and then stop. If repairs stall, check the trace:

```bash
fixit file.cpp --agent --llm openai --base-url ... --model ... \
      --api-key ollama --trace trace.json --no-write
```

`tool_calls` in the trace shows whether the model asked for `patch` or only
answered with prose.

## 5. Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| `this build has no TLS support` | `--base-url` points at `https://` | use the local model (plain http) or rebuild with `-DFIXIT_ENABLE_OPENSSL=ON` |
| `model not found` | the tag is not pulled | `ollama pull <model>` |
| Empty tool calls every round | the model cannot do tool calling | pick a coder model, or add `--iterations 1` to see one round of the failure report |
| Very slow first round | CPU inference / model loading | warm the model with `ollama run <model> "hi"` before the repair |
