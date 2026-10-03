// multi_error.cpp -- two independent errors in one translation unit.
// Used by n01_multi_error.clang (real capture) and n08_json_multi_error.clang
// (the same diagnostics serialised in clang's JSON schema).
int main() {
  missing_symbol_a();
  missing_symbol_b();
  return 0;
}
