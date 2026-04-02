#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace mailfs::core::model {

struct AppConfig {
  std::string imap_host = "imap.qq.com";
  std::uint16_t imap_port = 993;
  std::string credential_file = "passwd.txt";
  std::string ca_cert_file;
  std::string log_level = "info";
  std::string log_file = "mailfs.log";
  bool log_to_stderr = true;
  std::size_t log_max_file_size = 10u * 1024u * 1024u;
  int log_max_files = 5;
  std::string email_name = "mailfs";
  std::string owner_name = "sunshine";
  std::string default_mailbox;
  std::string download_dir = "downloads";
  std::string http_listen_addr = ":9888";
  std::string http_copy_addr = "http://127.0.0.1:9888";
  std::string database_path = "mailfs_cache.db";
  std::size_t default_block_size = 512u * 65536u;
  std::size_t cache_fetch_batch_size = 32;
  std::unordered_map<std::string, std::size_t> block_sizes;
  std::vector<std::string> allowed_folders;
  std::vector<std::string> ignore_extensions;
  bool allow_insecure_tls = false;

  [[nodiscard]] std::size_t block_size_for_file(const std::filesystem::path& file) const;
  [[nodiscard]] bool is_allowed_mailbox(const std::string& mailbox) const;
  [[nodiscard]] bool should_ignore_file(const std::filesystem::path& file) const;
};

}  // namespace mailfs::core::model
