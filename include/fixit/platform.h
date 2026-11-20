#pragma once
// Platform helpers: UTF-8 at the process boundary, wide paths underneath.
//
// Everything FixIt exchanges with the outside world -- argv, compiler output, the
// model's tool arguments -- is UTF-8.  Two boundaries need an explicit
// conversion:
//
//   * std::filesystem::path's narrow constructor interprets its input in the
//     execution charset.  On Windows that is the ANSI code page, so a UTF-8 byte
//     sequence either throws ("Illegal byte sequence") or names a different file
//     than the one the user asked for.  to_path()/from_path() go through the
//     code-unit sequence instead, which is defined on every platform.
//   * a compiler on Windows writes diagnostics in the console code page, not
//     UTF-8.  captured_text_to_utf8() repairs output that would otherwise arrive
//     as mojibake -- and a header path in a diagnostic would never match a file
//     on disk.

#include <filesystem>
#include <string>

namespace fixit {

/// UTF-8 -> filesystem path, without a locale round trip.
std::filesystem::path to_path(const std::string& utf8);

/// Filesystem path -> UTF-8, without a locale round trip.
std::string from_path(const std::filesystem::path& path);

/// Returns compiler output as valid UTF-8.  Text that already is valid UTF-8 is
/// returned unchanged (clang emits UTF-8); on Windows anything else is treated as
/// ANSI text and converted.  A no-op on other platforms.
std::string captured_text_to_utf8(std::string text);

}  // namespace fixit
