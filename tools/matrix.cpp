// matrix.cpp -- measures how tolerant the patch engine actually is.
//
// This is the tool behind docs/patch_success_matrix.json and the Wiki heat map.
// It builds a family of well-formed unified diffs whose imperfection is
// controlled along two axes and reports the fraction the engine applies *to the
// right line*:
//
//   impairment  what is wrong with the quoted context
//   drift       how far the @@ header is from the truth, in lines
//
// Every case is generated deterministically, so the JSON is reproducible and the
// numbers in the README/wiki cannot drift away from the code.
//
// Usage: fixit-matrix [output.json]

#include <cstddef>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "fixit/patch.h"

namespace {

/// The corpus deliberately contains realistic ambiguity: `}` and blank lines
/// repeat on every eighth line, and the *neighbour* declarations repeat too.  A
/// hunk quoting only the changed line plus one context line therefore has several
/// plausible homes, which is precisely the situation the sliding window exists
/// for.  Only the `local_<i>` line is unique, so a wrong placement is
/// detectable.
std::vector<std::string> make_base(int functions) {
  std::vector<std::string> lines;
  lines.push_back("#include <cstdio>");
  lines.push_back("#include <string>");
  lines.push_back("");
  for (int i = 0; i < functions; ++i) {
    lines.push_back("int function_" + std::to_string(i) + "(int input) {");
    lines.push_back("  int local_" + std::to_string(i) + " = input + " + std::to_string(i) + ";");
    lines.push_back("  return local_" + std::to_string(i) + " * 2;");
    lines.push_back("}");
    lines.push_back("");
  }
  return lines;
}

/// A third corpus where every block is byte-identical, including the line being
/// repaired.  No amount of context can identify the intended one, so this is the
/// genuine limit of fuzzy matching rather than a tuning problem.
std::vector<std::string> make_ambiguous(int functions) {
  std::vector<std::string> lines;
  lines.push_back("#include <cstdio>");
  lines.push_back("#include <string>");
  lines.push_back("");
  for (int i = 0; i < functions; ++i) {
    lines.push_back("int handler_" + std::to_string(i) + "(int input) {");
    lines.push_back("  if (input < 0) return 0;");
    lines.push_back("  int work_" + std::to_string(i) + " = input;");
    lines.push_back("  return work_" + std::to_string(i) + ";");
    lines.push_back("}");
    lines.push_back("");
  }
  return lines;
}

/// Every block here is identical, so the quoted context cannot distinguish the
/// candidates: only the distance to the declared position can.
std::vector<std::string> make_identical(int functions) {
  std::vector<std::string> lines;
  lines.push_back("#include <cstdio>");
  lines.push_back("#include <string>");
  lines.push_back("");
  for (int i = 0; i < functions; ++i) {
    lines.push_back("int handler(int input) {");
    lines.push_back("  if (input < 0) return 0;");
    lines.push_back("  int work = input;");
    lines.push_back("  return work;");
    lines.push_back("}");
    lines.push_back("");
  }
  return lines;
}

std::string render(const std::vector<std::string>& lines) {
  std::string out;
  for (std::size_t i = 0; i < lines.size(); ++i) {
    out += lines[i];
    if (i + 1 < lines.size()) out += '\n';
  }
  if (!lines.empty()) out += '\n';
  return out;
}

/// Trailing-blank copy of the whole file (a model that stripped whitespace).
std::vector<std::string> padded(const std::vector<std::string>& lines) {
  std::vector<std::string> out;
  out.reserve(lines.size());
  for (const std::string& line : lines) out.push_back(line.empty() ? line : line + "   ");
  return out;
}

enum class Impairment { Exact, TrailingWhitespace, MissingContext, ExtraContext };

std::string impairment_name(Impairment value) {
  switch (value) {
    case Impairment::Exact: return "exact";
    case Impairment::TrailingWhitespace: return "trailing ws";
    case Impairment::MissingContext: return "missing ctx";
    case Impairment::ExtraContext: return "extra ctx";
  }
  return "?";
}

struct Case {
  bool applied = false;        ///< all_applied
  bool exactly_right = false;  ///< the repaired line replaced the right line
  double score = 0;
};

/// Builds one case and runs it through the engine.
///
/// The generated diff is always *well formed*: the `@@` counts match the lines
/// actually quoted, exactly as a real unified diff must.  The impairment is in
/// the shape of the context, never in the header arithmetic -- a malformed
/// header is a different (and uninteresting) failure mode.
Case run_case(const std::vector<std::string>& file_lines, int target, int context, Impairment kind,
              int drift) {
  Case result;
  // Block `target` starts at 0-based 3 + 4*target; the line under repair is
  // its second line (the `local_*` / `work_*` assignment), i.e. +1.
  const int target_index = 3 + 4 * target + 1;
  if (target_index < 0 || static_cast<std::size_t>(target_index) >= file_lines.size()) return result;

  int quote_begin = target_index - context;
  int quote_end = target_index + context;
  while (quote_begin < 0 || quote_end >= static_cast<int>(file_lines.size())) {
    if (quote_begin < 0) ++quote_begin;
    if (quote_end >= static_cast<int>(file_lines.size())) --quote_end;
    if (quote_begin >= quote_end) break;
  }

  if (kind == Impairment::MissingContext) {
    const int trim = std::max(1, context / 2);
    quote_begin += trim;
    quote_end -= trim;
    if (quote_begin > target_index) quote_begin = target_index;
    if (quote_end < target_index) quote_end = target_index;
  } else if (kind == Impairment::ExtraContext && quote_begin > 0) {
    --quote_begin;  // one context line more than the window, still well formed
  }

  const std::string repaired = "  int repaired_" + std::to_string(target) + " = 1;";
  const bool trailing_ws = kind == Impairment::TrailingWhitespace;

  std::string body;
  int quoted_lines = 0;
  for (int index = quote_begin; index <= quote_end; ++index) {
    const std::string& line = file_lines[static_cast<std::size_t>(index)];
    const std::string suffix = trailing_ws ? " " : "";
    if (index == target_index) {
      body += "-" + line + suffix + "\n";
      body += "+" + repaired + suffix + "\n";
    } else {
      body += " " + line + suffix + "\n";
    }
    ++quoted_lines;
  }
  if (quoted_lines == 0) return result;

  const int declared_start = quote_begin + 1 + drift;
  std::ostringstream diff;
  diff << "--- a/matrix.cpp\n+++ b/matrix.cpp\n";
  diff << "@@ -" << declared_start << "," << quoted_lines << " +" << declared_start << ","
       << quoted_lines << " @@\n";
  diff << body;

  const fixit::PatchEngine engine;
  const fixit::PatchResult applied = engine.apply(render(file_lines), diff.str(), "matrix.cpp");
  result.applied = applied.all_applied;

  const std::size_t found = applied.new_content.find(repaired);
  if (found != std::string::npos) {
    const int line = 1 + static_cast<int>(std::count(applied.new_content.begin(),
                                                    applied.new_content.begin() +
                                                        static_cast<std::ptrdiff_t>(found),
                                                    '\n'));
    result.exactly_right = line == target_index + 1;
  }
  for (const fixit::HunkReport& report : applied.reports) {
    result.score = std::max(result.score, report.score);
  }
  return result;
}

std::string json_escape(const std::string& text) {
  std::string out;
  for (char c : text) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else {
      out += c;
    }
  }
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string output = argc > 1 ? argv[1] : "patch_success_matrix.json";

  const int kFunctions = 220;  // 1103 lines: every declared drift stays in range
  const int kContext = 3;
  const std::vector<std::string> base = make_base(kFunctions);
  const std::vector<std::string> whitespace = padded(base);
  // Every drift here keeps the declared position inside the file (an
  // out-of-range anchor is a different, uninteresting failure mode).
  const std::vector<int> drifts = {0, 1, 2, 5, 10, 20, 50};
  const std::vector<Impairment> impairments = {Impairment::Exact, Impairment::TrailingWhitespace,
                                               Impairment::MissingContext,
                                               Impairment::ExtraContext};

  // -------------------------------------------------------------------------
  // The heat map: apply rate for each (impairment, drift) pair.  A case counts
  // only when the hunk applied *and* landed on the line it replaced -- a repair
  // on the wrong line is not a repair.
  // -------------------------------------------------------------------------
  std::cout << "patch apply rate by impairment and declared-position drift\n";
  std::cout << "  (context = " << kContext << " lines each side, " << base.size()
            << "-line file, drift in both directions)\n\n";

  struct GridCell {
    std::string impairment;
    int drift = 0;
    double rate = 0;
    int trials = 0;
  };
  std::vector<GridCell> grid;

  std::cout << "  " << std::setw(14) << "impairment";
  for (int drift : drifts) {
    std::cout << std::setw(8) << ((drift > 0 ? "+" : "") + std::to_string(drift));
  }
  std::cout << "\n";

  for (Impairment kind : impairments) {
    std::cout << "  " << std::setw(14) << impairment_name(kind);
    for (int drift : drifts) {
      int good = 0;
      int total = 0;
      for (int target = 10; target < kFunctions - 10; ++target) {
        const std::vector<std::string>& source =
            kind == Impairment::TrailingWhitespace ? whitespace : base;
        for (int direction : {1, -1}) {
          if (drift == 0 && direction < 0) continue;  // -0 is the same case
          const Case result = run_case(source, target, kContext, kind, drift * direction);
          if (result.applied && result.exactly_right) ++good;
          ++total;
        }
      }
      const double rate = total == 0 ? 0.0 : static_cast<double>(good) / total;
      std::ostringstream percent;
      percent << static_cast<int>(std::lround(rate * 100.0)) << "%";
      std::cout << std::setw(8) << percent.str();
      grid.push_back(GridCell{impairment_name(kind), drift, rate, total});
    }
    std::cout << "\n";
  }

  // -------------------------------------------------------------------------
  // The same sweep over a corpus whose context lines repeat, so the window has
  // to choose between several equally plausible candidates.
  // -------------------------------------------------------------------------
  const std::vector<std::string> ambiguous = make_ambiguous(kFunctions);
  std::cout << "\nshared-context corpus (" << ambiguous.size()
            << " lines, identical function bodies except the repaired line)\n\n";
  std::cout << "  " << std::setw(14) << "impairment";
  for (int drift : drifts) {
    std::cout << std::setw(8) << ((drift > 0 ? "+" : "") + std::to_string(drift));
  }
  std::cout << "\n";

  struct AmbiguousCell {
    std::string impairment;
    int drift = 0;
    double rate = 0;
  };
  std::vector<AmbiguousCell> ambiguous_grid;
  for (Impairment kind : impairments) {
    std::cout << "  " << std::setw(14) << impairment_name(kind);
    for (int drift : drifts) {
      int good = 0;
      int total = 0;
      for (int target = 10; target < kFunctions - 10; ++target) {
        const std::vector<std::string>& source =
            kind == Impairment::TrailingWhitespace ? padded(ambiguous) : ambiguous;
        for (int direction : {1, -1}) {
          if (drift == 0 && direction < 0) continue;
          const Case result = run_case(source, target, kContext, kind, drift * direction);
          if (result.applied && result.exactly_right) ++good;
          ++total;
        }
      }
      const double rate = total == 0 ? 0.0 : static_cast<double>(good) / total;
      std::ostringstream percent;
      percent << static_cast<int>(std::lround(rate * 100.0)) << "%";
      std::cout << std::setw(8) << percent.str();
      ambiguous_grid.push_back(AmbiguousCell{impairment_name(kind), drift, rate});
    }
    std::cout << "\n";
  }

  // -------------------------------------------------------------------------
  // Adversarial corpus: identical blocks, so the hunk is genuinely ambiguous and
  // only the declared position (via the distance tie-break) can choose.
  // -------------------------------------------------------------------------
  const std::vector<std::string> identical = make_identical(kFunctions);
  std::cout << "\nidentical-block corpus (" << identical.size()
            << " lines; the patch context matches every block equally)\n\n";
  std::cout << "  " << std::setw(14) << "impairment";
  for (int drift : drifts) {
    std::cout << std::setw(8) << ((drift > 0 ? "+" : "") + std::to_string(drift));
  }
  std::cout << "\n";

  struct IdenticalCell {
    std::string impairment;
    int drift = 0;
    double rate = 0;
  };
  std::vector<IdenticalCell> identical_grid;
  for (Impairment kind : impairments) {
    std::cout << "  " << std::setw(14) << impairment_name(kind);
    for (int drift : drifts) {
      int good = 0;
      int total = 0;
      for (int target = 10; target < kFunctions - 10; ++target) {
        const std::vector<std::string>& source =
            kind == Impairment::TrailingWhitespace ? padded(identical) : identical;
        for (int direction : {1, -1}) {
          if (drift == 0 && direction < 0) continue;
          const Case result = run_case(source, target, kContext, kind, drift * direction);
          if (result.applied && result.exactly_right) ++good;
          ++total;
        }
      }
      const double rate = total == 0 ? 0.0 : static_cast<double>(good) / total;
      std::ostringstream percent;
      percent << static_cast<int>(std::lround(rate * 100.0)) << "%";
      std::cout << std::setw(8) << percent.str();
      identical_grid.push_back(IdenticalCell{impairment_name(kind), drift, rate});
    }
    std::cout << "\n";
  }

  double rate_sum = 0.0;
  for (const GridCell& cell : grid) rate_sum += cell.rate;
  const double mean_rate = grid.empty() ? 0.0 : rate_sum / static_cast<double>(grid.size());
  const double worst_rate = [&] {
    double worst = 1.0;
    for (const GridCell& cell : grid) worst = std::min(worst, cell.rate);
    return grid.empty() ? 0.0 : worst;
  }();

  std::cout << "\n  trials per cell: " << (grid.empty() ? 0 : grid.front().trials)
            << "   mean apply rate: " << static_cast<int>(std::lround(mean_rate * 100.0))
            << "%   worst cell: " << static_cast<int>(std::lround(worst_rate * 100.0)) << "%\n";
  std::cout << "  legend: 'trailing ws' = every quoted line carries an extra trailing space,\n"
               "          'missing ctx' = fewer context lines quoted than the window,\n"
               "          'extra ctx'   = one context line more than the window\n";

  // -------------------------------------------------------------------------
  // JSON for docs/patch_success_matrix.json and the heat-map renderer.
  // -------------------------------------------------------------------------
  std::ofstream out(output, std::ios::binary | std::ios::trunc);
  if (!out) {
    std::cerr << "fixit-matrix: cannot write " << output << "\n";
    return 1;
  }
  out << "{\n";
  out << "  \"generated_by\": \"tools/matrix.cpp\",\n";
  out << "  \"base_file_lines\": " << base.size() << ",\n";
  out << "  \"context_lines\": " << kContext << ",\n";
  out << "  \"config\": {\"max_drift\": 200, \"fuzzy_line_threshold\": 0.8, \"gate\": 0.8},\n";
  out << "  \"drifts\": [";
  for (std::size_t i = 0; i < drifts.size(); ++i) {
    out << drifts[i] << (i + 1 < drifts.size() ? ", " : "");
  }
  out << "],\n";
  out << "  \"impairments\": [";
  for (std::size_t i = 0; i < impairments.size(); ++i) {
    out << "\"" << json_escape(impairment_name(impairments[i])) << "\""
        << (i + 1 < impairments.size() ? ", " : "");
  }
  out << "],\n";
  out << "  \"mean_apply_rate\": " << std::fixed << std::setprecision(4) << mean_rate << ",\n";
  out << "  \"grid\": [\n";
  for (std::size_t i = 0; i < grid.size(); ++i) {
    out << "    {\"impairment\": \"" << json_escape(grid[i].impairment)
        << "\", \"drift\": " << grid[i].drift << ", \"apply_rate\": " << std::fixed
        << std::setprecision(4) << grid[i].rate << "}" << (i + 1 < grid.size() ? "," : "") << "\n";
  }
  out << "  ],\n";
  out << "  \"ambiguous_grid\": [\n";
  for (std::size_t i = 0; i < ambiguous_grid.size(); ++i) {
    out << "    {\"impairment\": \"" << json_escape(ambiguous_grid[i].impairment)
        << "\", \"drift\": " << ambiguous_grid[i].drift << ", \"apply_rate\": " << std::fixed
        << std::setprecision(4) << ambiguous_grid[i].rate
        << "}" << (i + 1 < ambiguous_grid.size() ? "," : "") << "\n";
  }
  out << "  ],\n";
  out << "  \"identical_grid\": [\n";
  for (std::size_t i = 0; i < identical_grid.size(); ++i) {
    out << "    {\"impairment\": \"" << json_escape(identical_grid[i].impairment)
        << "\", \"drift\": " << identical_grid[i].drift << ", \"apply_rate\": " << std::fixed
        << std::setprecision(4) << identical_grid[i].rate
        << "}" << (i + 1 < identical_grid.size() ? "," : "") << "\n";
  }
  out << "  ]\n";
  out << "}\n";
  out.close();

  std::cout << "\nwrote " << output << "\n";
  return 0;
}
