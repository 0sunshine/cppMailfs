#pragma once

#include <string>
#include <vector>

namespace mailfs::infra::platform {

void prepare_console_utf8();

std::vector<std::string> argv_to_utf8(int argc, char** argv);

#ifdef _WIN32
std::vector<std::string> argv_to_utf8(int argc, wchar_t** argv);
#endif

}  // namespace mailfs::infra::platform
