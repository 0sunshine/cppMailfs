#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace mailfs::core {

std::string md5_hex(const std::vector<unsigned char>& bytes);
std::string md5_hex(const std::filesystem::path& file_path);

}  // namespace mailfs::core
