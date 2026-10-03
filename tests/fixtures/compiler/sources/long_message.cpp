// long_message.cpp -- a static_assert whose text is far wider than a terminal.
static_assert(sizeof(void*) == 16,
              "fixit fixture: this deliberately enormous diagnostic message exists so that "
              "the parser's message capture can be compared character for character; any "
              "truncation, line folding or re-wrapping by the parser would be obvious here");
int main() { return 0; }
