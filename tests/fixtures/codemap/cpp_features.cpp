#include <vector>

template <typename T>
T clamp_value(T value, T low, T high) {
  if (value < low) return low;
  if (value > high) return high;
  return value;
}

auto make_adder(int base) {
  return [base](int value) { return base + value; };
}

struct Point {
  int x = 0;
  int y = 0;
};

int main() {
  auto adder = make_adder(3);
  return adder(4);
}
