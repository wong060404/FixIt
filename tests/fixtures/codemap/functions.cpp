#include <string>
#include <vector>
#include "helper.h"

int free_function(int a, int b) {
  return a + b;
}

static double scaled(double value) {
  return value * 2.0;
}

namespace outer {
namespace inner {

class Widget {
 public:
  Widget() = default;
  void draw() const {
    int local = 1;
  }
  int width() { return width_; }

 private:
  int width_ = 0;
};

std::string describe(const Widget& widget) {
  return "widget";
}

}  // namespace inner
}  // namespace outer

int main(int argc, char** argv) {
  Widget widget;
  return free_function(1, 2);
}
