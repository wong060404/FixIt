// caret_context.cpp -- the message is followed by a caret block.
int compute(int lhs, int rhs) { return lhs + rhs; }

int main() {
  int result = compute(1, "two");
  return result;
}
