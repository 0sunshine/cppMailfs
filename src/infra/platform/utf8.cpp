#include "mailfs/infra/platform/utf8.hpp"

#include <clocale>
#include <codecvt>
#include <cwchar>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#include <Windows.h>
#else
#include <cwchar>
#include <locale>
#endif

namespace mailfs::infra::platform {

namespace {

#ifdef _WIN32
std::string wide_to_utf8(const wchar_t* text) {
  if (text == nullptr) {
    return {};
  }

  const auto wide_length = static_cast<int>(wcslen(text));
  if (wide_length == 0) {
    return {};
  }

  const auto utf8_length = WideCharToMultiByte(CP_UTF8, 0, text, wide_length, nullptr, 0, nullptr, nullptr);
  if (utf8_length <= 0) {
    throw std::runtime_error("WideCharToMultiByte failed");
  }

  std::string utf8(static_cast<std::size_t>(utf8_length), '\0');
  if (WideCharToMultiByte(CP_UTF8,
                          0,
                          text,
                          wide_length,
                          utf8.data(),
                          utf8_length,
                          nullptr,
                          nullptr) != utf8_length) {
    throw std::runtime_error("WideCharToMultiByte failed");
  }

  return utf8;
}
#else
std::string wide_to_utf8(const std::wstring& text) {
  std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
  return converter.to_bytes(text);
}

std::string multibyte_to_utf8(const char* text) {
  if (text == nullptr) {
    return {};
  }

  std::setlocale(LC_ALL, "");

  std::mbstate_t state{};
  const char* src = text;
  const auto wide_length = std::mbsrtowcs(nullptr, &src, 0, &state);
  if (wide_length == static_cast<std::size_t>(-1)) {
    return text;
  }

  std::wstring wide(wide_length, L'\0');
  state = std::mbstate_t{};
  src = text;
  if (std::mbsrtowcs(wide.data(), &src, wide.size(), &state) == static_cast<std::size_t>(-1)) {
    return text;
  }

  return wide_to_utf8(wide);
}
#endif

}  // namespace

void prepare_console_utf8() {
#ifdef _WIN32
  SetConsoleCP(CP_UTF8);
  SetConsoleOutputCP(CP_UTF8);
#else
  std::setlocale(LC_ALL, "");
#endif
}

std::vector<std::string> argv_to_utf8(int argc, char** argv) {
  std::vector<std::string> args;
  args.reserve(static_cast<std::size_t>(argc));
  for (int i = 0; i < argc; ++i) {
#ifdef _WIN32
    args.emplace_back(argv[i] != nullptr ? argv[i] : "");
#else
    args.push_back(multibyte_to_utf8(argv[i]));
#endif
  }
  return args;
}

#ifdef _WIN32
std::vector<std::string> argv_to_utf8(int argc, wchar_t** argv) {
  std::vector<std::string> args;
  args.reserve(static_cast<std::size_t>(argc));
  for (int i = 0; i < argc; ++i) {
    args.push_back(wide_to_utf8(argv[i]));
  }
  return args;
}
#endif

}  // namespace mailfs::infra::platform
