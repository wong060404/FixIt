#include "fixit/platform.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace fixit {

#if defined(_WIN32)
namespace {

/// True when `text` decodes as UTF-8 (surrogate pairs and overlong forms are not
/// re-validated: this only has to distinguish "the compiler wrote UTF-8" from
/// "the compiler wrote the ANSI code page").
bool is_valid_utf8(const std::string& text) {
  std::size_t i = 0;
  while (i < text.size()) {
    const unsigned char lead = static_cast<unsigned char>(text[i]);
    std::size_t extra = 0;
    if (lead < 0x80) {
      ++i;
      continue;
    }
    if ((lead & 0xE0) == 0xC0) {
      if (lead < 0xC2) return false;  // overlong two-byte form
      extra = 1;
    } else if ((lead & 0xF0) == 0xE0) {
      extra = 2;
    } else if ((lead & 0xF8) == 0xF0) {
      if (lead > 0xF4) return false;
      extra = 3;
    } else {
      return false;
    }
    if (i + extra >= text.size()) return false;
    for (std::size_t k = 1; k <= extra; ++k) {
      if ((static_cast<unsigned char>(text[i + k]) & 0xC0) != 0x80) return false;
    }
    i += extra + 1;
  }
  return true;
}

std::string ansi_to_utf8(const std::string& ansi) {
  const int wide_length =
      ::MultiByteToWideChar(CP_ACP, 0, ansi.data(), static_cast<int>(ansi.size()), nullptr, 0);
  if (wide_length <= 0) return ansi;
  std::wstring wide(static_cast<std::size_t>(wide_length), L'\0');
  if (::MultiByteToWideChar(CP_ACP, 0, ansi.data(), static_cast<int>(ansi.size()), wide.data(),
                            wide_length) != wide_length) {
    return ansi;
  }
  const int utf8_length = ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), wide_length, nullptr, 0,
                                                nullptr, nullptr);
  if (utf8_length <= 0) return ansi;
  std::string utf8(static_cast<std::size_t>(utf8_length), '\0');
  if (::WideCharToMultiByte(CP_UTF8, 0, wide.data(), wide_length, utf8.data(), utf8_length, nullptr,
                            nullptr) != utf8_length) {
    return ansi;
  }
  return utf8;
}

}  // namespace
#endif  // _WIN32

std::filesystem::path to_path(const std::string& utf8) {
#if defined(__cpp_lib_char8_t)
  return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
#else
  return std::filesystem::u8path(utf8);
#endif
}

std::string from_path(const std::filesystem::path& path) {
#if defined(__cpp_lib_char8_t)
  const std::u8string utf8 = path.u8string();
  return std::string(utf8.begin(), utf8.end());
#else
  return path.u8string();
#endif
}

std::string captured_text_to_utf8(std::string text) {
#if defined(_WIN32)
  if (text.empty() || is_valid_utf8(text)) return text;
  return ansi_to_utf8(text);
#else
  return text;
#endif
}

}  // namespace fixit
