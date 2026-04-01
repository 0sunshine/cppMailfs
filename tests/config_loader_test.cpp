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
  EXPECT_EQ(config.database_path, "cache.db");
  EXPECT_EQ(config.default_block_size, 4096u);
  ASSERT_TRUE(config.block_sizes.count(".mp4"));
  EXPECT_EQ(config.block_sizes.at(".mp4"), 8192u);
  ASSERT_EQ(config.ignore_extensions.size(), 1u);
  EXPECT_EQ(config.ignore_extensions.front(), ".tmp");

  std::filesystem::remove(path);
}
