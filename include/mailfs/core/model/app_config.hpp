#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace mailfs::core::model {

struct AppConfig {
  std::string imap_host = "imap.qq.com";
  std::uint16_t imap_port = 993;
  std::string credential_file = "passwd.txt";
  std::string ca_cert_file;
  std::string email_name = "mailfs";
  std::string mailbox_prefix = "*";
  std::string database_path = "mailfs_cache.db";
  std::size_t default_block_size = 512u * 65536u;
  std::unordered_map<std::string, std::size_t> block_sizes;
  std::vector<std::string> allowed_folders;
  std::vector<std::string> ignore_extensions;
  bool allow_insecure_tls = false;

  [[nodiscard]] std::size_t block_size_for_file(const std::filesystem::path& file) const;
  [[nodiscard]] bool is_allowed_mailbox(const std::string& mailbox) const;
  [[nodiscard]] bool should_ignore_file(const std::filesystem::path& file) const;
};

}  // namespace mailfs::core::model
