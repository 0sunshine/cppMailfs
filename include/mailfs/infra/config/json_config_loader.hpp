#pragma once

#include <filesystem>

#include "mailfs/core/model/app_config.hpp"

namespace mailfs::infra::config {

class JsonConfigLoader {
 public:
  static core::model::AppConfig load(const std::filesystem::path& path);
};

}  // namespace mailfs::infra::config
