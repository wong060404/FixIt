#include "fixit/patch.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fixit {
namespace {

// ---------------------------------------------------------------------------
// Line helpers
// ---------------------------------------------------------------------------

/// Splits into lines, dropping the trailing empty element produced by a final
/// newline.  A file that does not end with a newline yields its last line too.
std::vector<std::string> split_keep_tail(const std::string& content) {
  std::vector<std::string> lines;
  std::string current;
  for (char c : content) {
    if (c == '\n') {
      lines.push_back(current);
      current.clear();
    } else {
      current.push_back(c);
    }
  }
  if (!current.empty()) lines.push_back(current);
  return lines;
}

std::string join_lines(const std::vector<std::string>& lines) {
  std::string out;
  for (std::size_t i = 0; i < lines.size(); ++i) {
    out += lines[i];
    if (i + 1 < lines.size()) out += '\n';
  }
  // The original file's trailing-newline state is preserved by the caller, so
  // here we simply reproduce `a\nb` (no trailing newline).
  return out;
}

bool ends_with_newline(const std::string& s) { return !s.empty() && s.back() == '\n'; }

std::string basename_of(const std::string& path) {
  std::string p = path;
  // strip a/ or b/ prefix used by git-style diffs
  if (p.size() > 2 && (p[0] == 'a' || p[0] == 'b') && p[1] == '/') p = p.substr(2);
  const std::size_t slash = p.find_last_of("/\\");
  return slash == std::string::npos ? p : p.substr(slash + 1);
}

/// Strips the trailing timestamp git appends to `---`/`+++` lines.
std::string strip_timestamp(std::string path) {
  const std::size_t tab = path.find('\t');
  if (tab != std::string::npos) path = path.substr(0, tab);
  return path;
}

std::string trim(const std::string& s) {
  std::size_t b = 0;
  std::size_t e = s.size();
  while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
  return s.substr(b, e - b);
}

std::string format_line(const std::string& s) {
  std::string out;
  for (char c : s) {
    if (c == '\t') {
      out += "\\t";
    } else {
      out.push_back(c);
    }
  }
  return out;
}

std::string describe_score(double score) {
  std::ostringstream os;
  os.setf(std::ios::fixed);
  os.precision(2);
  os << score;
  return os.str();
}

// ---------------------------------------------------------------------------
// Candidate evaluation
//
// Position convention used throughout this file: candidate positions are
// 0-based indices into the file's line vector.  A candidate `pos` means the
// signature would occupy file lines [pos, pos + signature.size()).  The
// 1-based line numbers that users see are derived only when a report is built
// (`pos + 1`).  Keeping one convention in the search path avoids the classic
// off-by-one between "the line the model wrote" and "the line in the file".
// ---------------------------------------------------------------------------
struct Evaluation {
  int exact = 0;
  int fuzzy = 0;
  int mismatch = 0;
  double score = 0;
  /// Index (0-based) into the signature of the first line that neither matched
  /// exactly nor fuzzily; -1 when every line matched.
  int first_mismatch_index = -1;
};

Evaluation evaluate_candidate(const std::vector<std::string>& file_lines, std::size_t pos,
                              const std::vector<std::string>& signature,
                              const std::vector<std::string>& normalized_signature,
                              double fuzzy_threshold) {
  Evaluation e;
  const std::size_t n = signature.size();
  for (std::size_t i = 0; i < n; ++i) {
    const std::size_t index = pos + i;
    const std::string found =
        index < file_lines.size() ? normalize_line(file_lines[index]) : std::string();
    if (normalized_signature[i] == found) {
      ++e.exact;
      continue;
    }
    if (levenshtein_ratio(normalized_signature[i], found) >= fuzzy_threshold) {
      ++e.fuzzy;
      continue;
    }
    ++e.mismatch;
    if (e.first_mismatch_index < 0) e.first_mismatch_index = static_cast<int>(i);
  }
  if (n == 0) {
    e.score = 1.0;
  } else {
    // Raw weights: exact 0.6, fuzzy 0.3, mismatch -0.2.
    const double raw = 0.6 * static_cast<double>(e.exact) + 0.3 * static_cast<double>(e.fuzzy) -
                       0.2 * static_cast<double>(e.mismatch);
    // Normalised by the best score the hunk could reach, so a perfect match is
    // 1.00 -- which is what the spec's demo transcript prints ("exact, score
    // 1.00") and what makes the documented gate of 0.80 reachable.  The literal
    // formula (raw / signature.size()) tops out at 0.60 even for a perfect
    // hunk, so every hunk would fail its own gate.  See docs/decisions.md
    // ADR-002.
    const double maximum = 0.6 * static_cast<double>(n);
    e.score = maximum > 0.0 ? raw / maximum : 0.0;
  }
  return e;
}

struct SearchOutcome {
  bool found = false;
  Candidate best;  ///< best.pos is 0-based
  Evaluation best_eval;
  std::size_t declared_pos = 0;  ///< 0-based
  /// Largest window radius actually scanned; max_drift means the global scan ran.
  int tried_max = 0;
};

const std::vector<int>& window_ladder() {
  static const std::vector<int> kWindows{0, 1, 2, 5, 10, 50};
  return kWindows;
}

SearchOutcome search_hunk(const std::vector<std::string>& file_lines, std::size_t anchor,
                          const std::vector<std::string>& signature,
                          const std::vector<std::string>& normalized_signature,
                          const PatchConfig& cfg, bool global_fallback) {
  SearchOutcome out;
  out.declared_pos = anchor;

  std::set<std::size_t> tried;
  bool evaluated_any = false;

  const auto valid = [&](std::size_t pos) {
    if (pos > file_lines.size()) return false;
    if (signature.empty()) return true;  // pure insertion: any boundary is fine
    return pos + signature.size() <= file_lines.size();
  };

  const auto consider = [&](std::size_t pos) {
    if (!valid(pos)) return;
    if (!tried.insert(pos).second) return;
    evaluated_any = true;

    const Evaluation e = evaluate_candidate(file_lines, pos, signature, normalized_signature,
                                            cfg.fuzzy_line_threshold);
    const Candidate cand{static_cast<int>(pos), e.score, e.exact, e.fuzzy, e.mismatch};
    const auto distance = [&](std::size_t p) {
      return p > anchor ? p - anchor : anchor - p;
    };
    const bool better =
        !out.found || e.score > out.best.score + 1e-12 ||
        (std::abs(e.score - out.best.score) <= 1e-12 &&
         (e.exact > out.best.exact ||
          (e.exact == out.best.exact && distance(pos) < distance(static_cast<std::size_t>(out.best.pos))) ||
          (e.exact == out.best.exact &&
           distance(pos) == distance(static_cast<std::size_t>(out.best.pos)) &&
           pos < static_cast<std::size_t>(out.best.pos))));
    if (better) {
      out.best = cand;
      out.best_eval = e;
      out.found = true;
    }
  };

  for (int radius : window_ladder()) {
    out.tried_max = radius;
    if (radius == 0) {
      consider(anchor);
    } else {
      const int low = std::max(0, static_cast<int>(anchor) - radius);
      const int high = static_cast<int>(anchor) + radius;
      for (int delta = -radius; delta <= radius; ++delta) {
        const int candidate = static_cast<int>(anchor) + delta;
        if (candidate < low || candidate > high) continue;
        if (candidate < 0) continue;
        consider(static_cast<std::size_t>(candidate));
      }
    }
    if (out.found && out.best.score >= 1.0) break;
  }

  if (global_fallback) {
    for (std::size_t pos = 0; pos <= file_lines.size(); ++pos) consider(pos);
    out.tried_max = cfg.max_drift;
  }

  if (!evaluated_any) out.found = false;
  return out;
}

// ---------------------------------------------------------------------------
// Diff parsing
// ---------------------------------------------------------------------------
struct HunkHeader {
  int old_start = 0;
  int old_count = 0;
  int new_start = 0;
  int new_count = 0;
};

std::optional<HunkHeader> parse_hunk_header(const std::string& line) {
  static const std::regex re(R"(^@@\s*-(\d+)(?:,(\d+))?\s*\+(\d+)(?:,(\d+))?\s*@@)");
  std::smatch m;
  if (!std::regex_search(line, m, re)) return std::nullopt;
  HunkHeader h;
  h.old_start = std::stoi(m[1].str());
  h.old_count = m[2].matched ? std::stoi(m[2].str()) : 1;
  h.new_start = std::stoi(m[3].str());
  h.new_count = m[4].matched ? std::stoi(m[4].str()) : 1;
  return h;
}

}  // namespace

// ---------------------------------------------------------------------------
// Public helpers
// ---------------------------------------------------------------------------
std::string normalize_line(const std::string& line) {
  std::size_t end = line.size();
  while (end > 0 && (line[end - 1] == ' ' || line[end - 1] == '\t' || line[end - 1] == '\r')) {
    --end;
  }
  return line.substr(0, end);
}

double levenshtein_ratio(const std::string& a, const std::string& b) {
  if (a == b) return 1.0;
  const std::size_t n = a.size();
  const std::size_t m = b.size();
  if (n == 0 || m == 0) return 0.0;

  std::vector<std::size_t> prev(m + 1);
  std::vector<std::size_t> curr(m + 1);
  for (std::size_t j = 0; j <= m; ++j) prev[j] = j;
  for (std::size_t i = 1; i <= n; ++i) {
    curr[0] = i;
    for (std::size_t j = 1; j <= m; ++j) {
      const std::size_t cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
      curr[j] = std::min({prev[j] + 1, curr[j - 1] + 1, prev[j - 1] + cost});
    }
    std::swap(prev, curr);
  }
  const std::size_t distance = prev[m];
  const std::size_t longest = std::max(n, m);
  return 1.0 - static_cast<double>(distance) / static_cast<double>(longest);
}

std::string diff_path_basename(const std::string& header_path) {
  return basename_of(strip_timestamp(trim(header_path)));
}

// ---------------------------------------------------------------------------
// parse_diff
// ---------------------------------------------------------------------------
std::vector<DiffFile> PatchEngine::parse_diff(const std::string& unified_diff) {
  std::vector<DiffFile> files;
  std::vector<std::string> lines = split_keep_tail(unified_diff);

  DiffFile* current = nullptr;
  HunkSpec* hunk = nullptr;

  for (std::size_t i = 0; i < lines.size(); ++i) {
    const std::string& raw = lines[i];

    if (raw.rfind("+++", 0) == 0 || raw.rfind("---", 0) == 0) {
      const std::string path = diff_path_basename(raw.substr(3));
      hunk = nullptr;
      if (raw.rfind("+++", 0) == 0) {
        if (path == "/dev/null" || path.empty()) {
          current = nullptr;
        } else {
          files.push_back(DiffFile{path, {}});
          current = &files.back();
        }
      } else if (path == "/dev/null") {
        // Deletion of the old file: the +++ side carries the real path.
        current = nullptr;
      }
      continue;
    }

    if (const std::optional<HunkHeader> header = parse_hunk_header(raw)) {
      if (current == nullptr) continue;
      current->hunks.push_back(HunkSpec{});
      hunk = &current->hunks.back();
      hunk->old_start = header->old_start;
      hunk->old_count = header->old_count;
      hunk->new_start = header->new_start;
      hunk->new_count = header->new_count;
      continue;
    }

    if (raw.rfind("\\", 0) == 0) continue;  // "\ No newline at end of file"
    if (hunk == nullptr) continue;

    if (raw.empty() || raw[0] == ' ') {
      const std::string text = raw.empty() ? std::string() : raw.substr(1);
      hunk->old_lines.push_back(text);
      hunk->new_lines.push_back(text);
    } else if (raw[0] == '-') {
      hunk->old_lines.push_back(raw.substr(1));
    } else if (raw[0] == '+') {
      hunk->new_lines.push_back(raw.substr(1));
    } else {
      // Anything else (e.g. `diff --git`) ends the hunk body.
      hunk = nullptr;
    }
  }

  return files;
}

// ---------------------------------------------------------------------------
// apply
// ---------------------------------------------------------------------------
PatchEngine::PatchEngine(PatchConfig cfg) : cfg_(cfg) {}

PatchResult PatchEngine::apply(const std::string& file_content, const std::string& unified_diff,
                               const std::string& target_file_hint) const {
  PatchResult result;
  result.new_content = file_content;

  std::vector<DiffFile> files = parse_diff(unified_diff);
  std::string file_label = target_file_hint;

  if (!target_file_hint.empty()) {
    const std::string want = diff_path_basename(target_file_hint);
    std::vector<DiffFile> filtered;
    for (const DiffFile& f : files) {
      if (basename_of(f.path) == want || f.path == target_file_hint) filtered.push_back(f);
    }
    files = std::move(filtered);
  }

  if (files.empty()) {
    result.all_applied = false;
    HunkReport report;
    report.hunk_index = 1;
    report.status = HunkReport::Status::Failed;
    report.failure_reason = "No hunk could be parsed from the supplied diff" +
                            (file_label.empty() ? std::string() : (" for " + file_label)) + ".";
    result.reports.push_back(std::move(report));
    return result;
  }
  if (file_label.empty()) file_label = files.front().path;

  std::vector<std::string> lines = split_keep_tail(file_content);
  const bool had_trailing_newline = ends_with_newline(file_content);
  int delta = 0;
  int hunk_counter = 0;
  bool all_applied = true;

  for (const DiffFile& file : files) {
    for (const HunkSpec& hunk : file.hunks) {
      ++hunk_counter;
      HunkReport report;
      report.hunk_index = hunk_counter;

      // 0-based anchor into the *current* line vector, shifted by the net line
      // change of every hunk applied so far.
      const long raw_anchor = static_cast<long>(hunk.old_start) - 1 + delta;
      const std::size_t anchor = raw_anchor < 0 ? 0 : static_cast<std::size_t>(raw_anchor);
      report.declared_pos = static_cast<int>(anchor) + 1;  // 1-based for humans

      std::vector<std::string> normalized_signature;
      normalized_signature.reserve(hunk.old_lines.size());
      for (const std::string& l : hunk.old_lines) normalized_signature.push_back(normalize_line(l));

      // --- pure insertion -------------------------------------------------
      if (hunk.old_lines.empty()) {
        std::size_t pos = anchor;
        if (pos > lines.size()) pos = lines.size();
        std::vector<std::string> spliced(lines.begin(), lines.begin() + static_cast<long>(pos));
        spliced.insert(spliced.end(), hunk.new_lines.begin(), hunk.new_lines.end());
        spliced.insert(spliced.end(), lines.begin() + static_cast<long>(pos), lines.end());
        lines = std::move(spliced);
        delta += static_cast<int>(hunk.new_lines.size());

        report.status = HunkReport::Status::Applied;
        report.matched_pos = static_cast<int>(pos) + 1;
        report.score = 1.0;
        result.reports.push_back(std::move(report));
        continue;
      }

      // --- anchored search with widening windows --------------------------
      SearchOutcome outcome =
          search_hunk(lines, anchor, hunk.old_lines, normalized_signature, cfg_, false);
      if (!outcome.found || outcome.best.score < cfg_.gate) {
        const SearchOutcome global =
            search_hunk(lines, anchor, hunk.old_lines, normalized_signature, cfg_, true);
        if (global.found && (!outcome.found || global.best.score > outcome.best.score + 1e-12)) {
          const int saved_max = outcome.tried_max;
          outcome.best = global.best;
          outcome.best_eval = global.best_eval;
          outcome.found = true;
          outcome.tried_max = cfg_.max_drift > saved_max ? cfg_.max_drift : saved_max;
        } else if (outcome.tried_max < cfg_.max_drift) {
          outcome.tried_max = cfg_.max_drift;  // the global scan did run
        }
      }

      const bool passes_gate =
          outcome.found && outcome.best.score >= cfg_.gate && outcome.best.exact >= 1;

      if (passes_gate) {
        const std::size_t pos = static_cast<std::size_t>(outcome.best.pos);
        // The hunk header is authoritative about how many file lines the hunk
        // consumes: a model that omits context still declares the true count.
        std::size_t count = hunk.old_count > 0 ? static_cast<std::size_t>(hunk.old_count)
                                               : hunk.old_lines.size();
        if (pos + count > lines.size()) count = lines.size() - pos;

        // Matching ignores trailing whitespace, so a hunk quoted from a
        // stripped copy must not silently reformat lines it merely passes
        // through.  Context lines take the file's own bytes; only genuinely new
        // ('+') lines become what the diff says.
        std::vector<std::string> replacement;
        replacement.reserve(count + 1);
        for (std::size_t i = 0; i < count; ++i) {
          const bool is_context = i < hunk.old_lines.size() && i < hunk.new_lines.size() &&
                                  hunk.new_lines[i] == hunk.old_lines[i];
          if (is_context) {
            replacement.push_back(lines[pos + i]);
          } else if (i < hunk.new_lines.size()) {
            replacement.push_back(hunk.new_lines[i]);
          }
        }

        std::vector<std::string> spliced(lines.begin(), lines.begin() + static_cast<long>(pos));
        spliced.insert(spliced.end(), replacement.begin(), replacement.end());
        spliced.insert(spliced.end(), lines.begin() + static_cast<long>(pos + count), lines.end());
        lines = std::move(spliced);
        delta += static_cast<int>(replacement.size()) - static_cast<int>(count);

        report.matched_pos = static_cast<int>(pos) + 1;
        report.score = outcome.best.score;
        report.status = (pos == anchor) ? HunkReport::Status::Applied
                                        : HunkReport::Status::FuzzyApplied;
        result.reports.push_back(std::move(report));
        continue;
      }

      // --- failure: explain exactly why -----------------------------------
      all_applied = false;
      report.status = HunkReport::Status::Failed;
      report.score = outcome.found ? outcome.best.score : 0.0;
      report.matched_pos = outcome.found ? outcome.best.pos + 1 : 0;

      std::vector<Candidate> candidates;
      if (outcome.found) candidates.push_back(outcome.best);
      for (std::size_t pos = 0; pos <= lines.size() && candidates.size() < 3; ++pos) {
        if (outcome.found && static_cast<std::size_t>(outcome.best.pos) == pos) continue;
        if (!hunk.old_lines.empty() && pos + hunk.old_lines.size() > lines.size()) continue;
        const Evaluation e = evaluate_candidate(lines, pos, hunk.old_lines, normalized_signature,
                                                cfg_.fuzzy_line_threshold);
        if (e.exact + e.fuzzy == 0) continue;
        candidates.push_back(Candidate{static_cast<int>(pos), e.score, e.exact, e.fuzzy, e.mismatch});
      }
      std::stable_sort(candidates.begin(), candidates.end(),
                       [](const Candidate& a, const Candidate& b) {
                         if (std::abs(a.score - b.score) > 1e-12) return a.score > b.score;
                         return a.exact > b.exact;
                       });
      if (candidates.size() > 3) candidates.resize(3);
      report.top_candidates = candidates;

      std::ostringstream reason;
      reason.setf(std::ios::fixed);
      reason.precision(2);
      reason << "score " << report.score << " < gate " << cfg_.gate;
      if (outcome.found) {
        reason << ". Declared position line " << report.declared_pos
               << " but the closest match is line " << report.matched_pos << " (score "
               << describe_score(outcome.best.score) << ", exact " << outcome.best.exact << "/"
               << hunk.old_lines.size() << ")";
      }
      reason << ". Window tried: +/-0..+/-" << outcome.tried_max << " plus global scan";
      if (outcome.found && outcome.best_eval.first_mismatch_index >= 0) {
        const std::size_t sig_index =
            static_cast<std::size_t>(outcome.best_eval.first_mismatch_index);
        const std::size_t file_index = static_cast<std::size_t>(outcome.best.pos) + sig_index;
        const std::string expected = sig_index < hunk.old_lines.size() ? hunk.old_lines[sig_index]
                                                                       : std::string();
        const std::string found =
            file_index < lines.size() ? lines[file_index] : std::string("<end of file>");
        reason << ". Context mismatch: expected line " << (file_index + 1) << " to contain '"
               << format_line(expected) << "' but the file has '" << format_line(found) << "'.";
      } else if (!outcome.found) {
        const std::string expected =
            hunk.old_lines.empty() ? std::string() : hunk.old_lines.front();
        reason << ". No position in the file can hold the hunk context; expected first line '"
               << format_line(expected) << "'.";
      }
      if (outcome.found && outcome.best.exact == 0) {
        reason << ". The best candidate matched only fuzzily (exact 0), which is below the "
                  "required exact >= 1.";
      }
      report.failure_reason = reason.str();
      result.reports.push_back(std::move(report));
    }
  }

  result.new_content = join_lines(lines);
  if (had_trailing_newline && !result.new_content.empty()) result.new_content += '\n';
  result.all_applied = all_applied && !result.reports.empty();
  return result;
}

// ---------------------------------------------------------------------------
// failure_summary
// ---------------------------------------------------------------------------
std::string PatchResult::failure_summary(const std::string& file_hint) const {
  std::ostringstream os;
  os.setf(std::ios::fixed);
  os.precision(2);

  if (all_applied) {
    // Nothing failed: summarise the application so callers can print progress.
    os << "Patch applied for " << (file_hint.empty() ? "<file>" : file_hint) << ":\n";
    for (const HunkReport& r : reports) {
      os << "- Hunk " << r.hunk_index << ": APPLIED @ line " << r.matched_pos << " ("
         << (r.status == HunkReport::Status::Applied ? "exact" : "fuzzy")
         << ", score " << r.score << ")\n";
    }
    return os.str();
  }

  os << "Patch failed for " << (file_hint.empty() ? "<file>" : file_hint) << ":\n";
  for (const HunkReport& r : reports) {
    os << "- Hunk " << r.hunk_index << ": ";
    switch (r.status) {
      case HunkReport::Status::Applied:
        os << "APPLIED @ line " << r.matched_pos << " (exact, score " << r.score << ")";
        break;
      case HunkReport::Status::FuzzyApplied:
        os << "APPLIED @ line " << r.matched_pos << " (fuzzy, drift "
           << (r.matched_pos - r.declared_pos) << ", score " << r.score << ")";
        break;
      case HunkReport::Status::Failed:
        os << "FAILED (" << r.failure_reason << "). Closest match at line " << r.matched_pos;
        break;
    }
    os << "\n";
    if (r.status == HunkReport::Status::Failed) {
      os << "  Suggested fix: ";
      if (!r.top_candidates.empty()) {
        const int center = r.top_candidates.front().pos;
        os << "re-read lines " << std::max(1, center - 3) << "-" << (center + 3)
           << " and retry with corrected context.";
      } else {
        os << "re-read the file around line " << std::max(1, r.declared_pos)
           << " and retry with corrected context.";
      }
      os << "\n";
    }
  }
  return os.str();
}

}  // namespace fixit
