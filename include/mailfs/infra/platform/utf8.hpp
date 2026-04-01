#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace mailfs::infra::platform {

void prepare_console_utf8();

std::vector<std::string> argv_to_utf8(int argc, char** argv);

#ifdef _WIN32
std::vector<std::string> argv_to_utf8(int argc, wchar_t** argv);
std::wstring utf8_to_wide(std::string_view text);
#endif

}  // namespace mailfs::infra::platform
