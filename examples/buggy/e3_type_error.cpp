// e3_type_error.cpp -- deliberately beyond the mock's rule set:
//   * an int is passed where a std::string is expected
//   * a function that does not exist is called
// The mock gives up here (R4); a real LLM is expected to have a go.
#include <string>

int count_words(const std::string& text) {
  return static_cast<int>(text.size());
}

int main() {
  int value = 42;
  int words = count_words(value);
  int extra = count_missing_words(value);
  return words + extra;
}
