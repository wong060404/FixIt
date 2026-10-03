// src/foo.cpp -- one directory down, so diagnostics carry a path prefix.
namespace fixit_fixture {
int broken() { return unknown_identifier; }
}  // namespace fixit_fixture
