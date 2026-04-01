#include "mailfs/core/model/app_config.hpp"

#include <algorithm>
#include <cctype>

namespace mailfs::core::model {

std::size_t AppConfig::block_size_for_file(const std::filesystem::path& file) const {
  auto ext = file.extension().u8string();
  std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });

  const auto it = block_sizes.find(ext);
  return it != block_sizes.end() ? it->second : default_block_size;
}

bool AppConfig::is_allowed_mailbox(const std::string& mailbox) const {
  if (allowed_folders.empty()) {
    return true;
  }

  return std::find(allowed_folders.begin(), allowed_folders.end(), mailbox) != allowed_folders.end();
}

bool AppConfig::should_ignore_file(const std::filesystem::path& file) const {
  auto name = file.filename().u8string();
  std::transform(name.begin(), name.end(), name.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });

  return std::any_of(ignore_extensions.begin(), ignore_extensions.end(), [&](const std::string& ext) {
    return name.size() >= ext.size() && name.compare(name.size() - ext.size(), ext.size(), ext) == 0;
  });
}

}  // namespace mailfs::core::model
