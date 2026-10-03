#pragma once
// fixit::PatchEngine -- the core of FixIt.
//
// LLM-produced unified diffs are almost never clean: line numbers drift, the
// context is incomplete, whitespace differs.  diffutils and patch(1) hard-fail
// on all three.  This engine scores every plausible location with a sliding
// window, applies the best one above a gate, and -- when nothing passes --
// returns a structured, human/LLM readable explanation of *why*.
//
// Partial-apply policy: hunks that pass the gate are written, hunks that do not
// keep their region untouched, and all_applied reports whether every hunk
// landed.  The caller therefore always gets the best available file state plus
// exact information about what is still missing.

#include <string>
#include <vector>

namespace fixit {

/// One `@@ -old_start,old_count +new_start,new_count @@` block.
struct HunkSpec {
  int old_start = 0;  ///< 1-based, as declared in the diff
  int old_count = 0;
  int new_start = 0;
  int new_count = 0;
  std::vector<std::string> old_lines;  ///< context + '-' lines, in file order
  std::vector<std::string> new_lines;  ///< context + '+' lines, in file order
};

struct DiffFile {
  std::string path;
  std::vector<HunkSpec> hunks;
};

/// Score of one candidate position, kept for failure reporting.
struct Candidate {
  int pos = 0;  ///< 1-based line where the hunk would start
  double score = 0;
  int exact = 0;
  int fuzzy = 0;
  int mismatch = 0;
};

struct HunkReport {
  int hunk_index = 0;  ///< 1-based
  int declared_pos = 0;
  int matched_pos = 0;
  double score = 0;
  enum class Status { Applied, FuzzyApplied, Failed };
  Status status = Status::Failed;
  std::string failure_reason;  ///< includes concrete line contents
  std::vector<Candidate> top_candidates;  ///< best 3, failures only
};

struct PatchResult {
  bool all_applied = false;
  std::string new_content;  ///< applied hunks written, failed hunks untouched
  std::vector<HunkReport> reports;

  /// Ready-to-feed-back text for the LLM.  See README for the exact shape.
  std::string failure_summary(const std::string& file_hint = "") const;
};

struct PatchConfig {
  int max_drift = 200;                ///< cap for the global fallback window
  double fuzzy_line_threshold = 0.8;  ///< per-line Levenshtein ratio
  double gate = 0.8;                  ///< minimum hunk score to apply
};

class PatchEngine {
 public:
  explicit PatchEngine(PatchConfig cfg = {});

  /// Applies `unified_diff` to `file_content`.  `target_file_hint` (when
  /// non-empty) selects one file out of a multi-file diff by basename match.
  PatchResult apply(const std::string& file_content,
                    const std::string& unified_diff,
                    const std::string& target_file_hint = "") const;

  static std::vector<DiffFile> parse_diff(const std::string& unified_diff);

  const PatchConfig& config() const { return cfg_; }

 private:
  PatchConfig cfg_;
};

/// Normalises one diff/file line for comparison: strips trailing whitespace
/// (including CR) and nothing else.  Exposed for tests.
std::string normalize_line(const std::string& line);

/// Levenshtein similarity in [0,1]: 1 - distance / max(len_a, len_b).
double levenshtein_ratio(const std::string& a, const std::string& b);

/// Extracts the path a diff header refers to (`+++ b/foo.cpp` -> `foo.cpp`).
std::string diff_path_basename(const std::string& header_path);

}  // namespace fixit
