// Minimal reuse example: the patch engine alone, no CLI and no agent.
//
// It demonstrates the library story from README section 8: link `FixIt::fixit`
// and drive `PatchEngine` directly.  Build it with the CMakeLists.txt next to
// this file (see the comment there for the exact commands).
#include <fixit/patch.h>

#include <iostream>
#include <string>

int main() {
  const std::string content = "int x = 1;\n";
  const std::string diff =
      "--- a/f.cpp\n+++ b/f.cpp\n@@ -1,1 +1,1 @@\n-int x = 1;\n+int x = 2;\n";

  fixit::PatchEngine engine;
  const fixit::PatchResult result = engine.apply(content, diff, "f.cpp");
  if (!result.all_applied) {
    std::cerr << result.failure_summary("f.cpp");
    return 1;
  }
  std::cout << result.new_content;
  return 0;
}
