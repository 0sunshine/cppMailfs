#include <filesystem>
#include <fstream>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "mailfs/infra/config/json_config_loader.hpp"

TEST(JsonConfigLoaderTest, LoadsOverridesAndNormalizesExtensions) {
  const auto path = std::filesystem::temp_directory_path() / "mailfs_config_test.json";
  std::ofstream output(path, std::ios::binary);
  const nlohmann::json config_json = {
      {"imap",
       {
           {"server", "imap.example.com:1993"},
           {"credential_file", "secret.txt"},
           {"ca_cert_file", "ca.pem"},
           {"allow_insecure_tls", true},
       }},
      {"logging",
       {
           {"level", "debug"},
           {"file", "trace/mailfs.log"},
           {"to_stderr", false},
           {"max_file_size", 2048},
           {"max_files", 7},
       }},
      {"identity", {{"owner_name", "sunshine"}}},
      {"mailbox", {{"default", u8"\u5176\u4ed6\u6587\u4ef6\u5939/codex\u6d4b\u8bd5"}}},
      {"storage",
       {
           {"download_dir", "downloads-root"},
           {"database_path", "cache.db"},
       }},
      {"http",
       {
           {"listen_addr", ":8765"},
           {"copy_addr", "http://127.0.0.1:8765"},
       }},
      {"cache",
       {
           {"default_block_size", 4096},
           {"block_sizes", { {".MP4", 8192} }},
       }},
      {"upload", {{"ignore_extensions", {".TMP"}}}},
  };
  output << config_json.dump(2);
  output.close();

  const auto config = mailfs::infra::config::JsonConfigLoader::load(path);
  EXPECT_EQ(config.imap_host, "imap.example.com");
  EXPECT_EQ(config.imap_port, 1993);
  EXPECT_EQ(config.credential_file, "secret.txt");
  EXPECT_EQ(config.ca_cert_file, "ca.pem");
  EXPECT_TRUE(config.allow_insecure_tls);
  EXPECT_EQ(config.log_level, "debug");
  EXPECT_EQ(config.log_file, "trace/mailfs.log");
  EXPECT_FALSE(config.log_to_stderr);
  EXPECT_EQ(config.log_max_file_size, 2048u);
  EXPECT_EQ(config.log_max_files, 7);
  EXPECT_EQ(config.owner_name, "sunshine");
  EXPECT_EQ(config.default_mailbox, u8"\u5176\u4ed6\u6587\u4ef6\u5939/codex\u6d4b\u8bd5");
  EXPECT_EQ(config.download_dir, "downloads-root");
  EXPECT_EQ(config.http_listen_addr, ":8765");
  EXPECT_EQ(config.http_copy_addr, "http://127.0.0.1:8765");
  EXPECT_EQ(config.database_path, "cache.db");
  EXPECT_EQ(config.default_block_size, 4096u);
  ASSERT_TRUE(config.block_sizes.count(".mp4"));
  EXPECT_EQ(config.block_sizes.at(".mp4"), 8192u);
  ASSERT_EQ(config.ignore_extensions.size(), 1u);
  EXPECT_EQ(config.ignore_extensions.front(), ".tmp");

  std::filesystem::remove(path);
}

TEST(JsonConfigLoaderTest, LoadsConfigFromUtf8NamedFile) {
  const auto path =
      std::filesystem::temp_directory_path() / std::filesystem::u8path(u8"\u90ae\u4ef6\u914d\u7f6e.json");
  std::ofstream output(path, std::ios::binary);
  const nlohmann::json config_json = {
      {"imap",
       {
           {"credential_file", u8"\u51ed\u636e.txt"},
           {"ca_cert_file", u8"\u6839\u8bc1\u4e66.pem"},
       }},
      {"logging", {{"file", u8"\u65e5\u5fd7/mailfs.log"}}},
      {"mailbox", {{"default", u8"\u5176\u4ed6\u6587\u4ef6\u5939/codex\u6d4b\u8bd5"}}},
      {"storage",
       {
           {"download_dir", u8"\u4e0b\u8f7d"},
           {"database_path", u8"\u7f13\u5b58.db"},
       }},
  };
  output << config_json.dump(2);
  output.close();

  const auto config = mailfs::infra::config::JsonConfigLoader::load(path);
  EXPECT_EQ(config.credential_file, u8"\u51ed\u636e.txt");
  EXPECT_EQ(config.ca_cert_file, u8"\u6839\u8bc1\u4e66.pem");
  EXPECT_EQ(config.log_file, u8"\u65e5\u5fd7/mailfs.log");
  EXPECT_EQ(config.default_mailbox, u8"\u5176\u4ed6\u6587\u4ef6\u5939/codex\u6d4b\u8bd5");
  EXPECT_EQ(config.download_dir, u8"\u4e0b\u8f7d");
  EXPECT_EQ(config.database_path, u8"\u7f13\u5b58.db");

  std::filesystem::remove(path);
}
