// clean.cpp -- deliberately well formed: the compiler must report nothing.
int add(int a, int b) { return a + b; }
int main() { return add(1, 2) == 3 ? 0 : 1; }
