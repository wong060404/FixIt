// e1_missing_include.cpp -- two injected bugs, both repaired by the mock rules:
//   1. std::vector is used without pulling in <vector>
//   2. the `int n = 3` line is missing its semicolon
// They sit in different functions so every compiler reports them in source
// order, and the missing ';' masks the header error on clang's first pass --
// which is exactly the situation the repair loop exists for.
#include <string>

#include <vector>
int parse_count(const std::string& text) {
  int n = 3;
  return n;
}

int add_word(std::vector<std::string>& words, const std::string& word) {
  words.push_back(word);
  return static_cast<int>(words.size());
}

int main() {
  std::vector<std::string> words;
  add_word(words, "x");
  return parse_count("x");
}
