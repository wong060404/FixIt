// warning_only.cpp -- compiles without errors, but not silently.
// Used by n02_warning_only.clang (real capture), n10_json_warning.clang (the
// same diagnostic serialised in clang's JSON schema) and n12_warning_only.gcc.
[[deprecated("use newer_api() instead")]] int old_api() { return 0; }

int main() { return old_api(); }
