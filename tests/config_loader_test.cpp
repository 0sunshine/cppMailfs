#include <filesystem>
#include <fstream>

#include <gtest/gtest.h>

#include "mailfs/infra/config/json_config_loader.hpp"

TEST(JsonConfigLoaderTest, LoadsOverridesAndNormalizesExtensions) {
  const auto path = std::filesystem::temp_directory_path() / "mailfs_config_test.json";
  std::ofstream output(path);
  output << R"({
    "imap_server": "imap.example.com:1993",
    "credential_file": "secret.txt",
    "ca_cert_file": "ca.pem",
    "log_level": "debug",
    "log_file": "trace/mailfs.log",
    "log_to_stderr": false,
    "database_path": "cache.db",
    "default_block_size": 4096,
    "block_sizes": {".MP4": 8192},
    "ignore_extensions": [".TMP"]
  })";
  output.close();

  const auto config = mailfs::infra::config::JsonConfigLoader::load(path);
  EXPECT_EQ(config.imap_host, "imap.example.com");
  EXPECT_EQ(config.imap_port, 1993);
  EXPECT_EQ(config.credential_file, "secret.txt");
  EXPECT_EQ(config.ca_cert_file, "ca.pem");
  EXPECT_EQ(config.log_level, "debug");
  EXPECT_EQ(config.log_file, "trace/mailfs.log");
  EXPECT_FALSE(config.log_to_stderr);
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
  output << R"({
    "credential_file": "\u51ed\u636e.txt",
    "ca_cert_file": "\u6839\u8bc1\u4e66.pem",
    "log_file": "\u65e5\u5fd7/mailfs.log",
    "database_path": "\u7f13\u5b58.db"
  })";
  output.close();

  const auto config = mailfs::infra::config::JsonConfigLoader::load(path);
  EXPECT_EQ(config.credential_file, u8"\u51ed\u636e.txt");
  EXPECT_EQ(config.ca_cert_file, u8"\u6839\u8bc1\u4e66.pem");
  EXPECT_EQ(config.log_file, u8"\u65e5\u5fd7/mailfs.log");
  EXPECT_EQ(config.database_path, u8"\u7f13\u5b58.db");

  std::filesystem::remove(path);
}
