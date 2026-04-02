#include <filesystem>
#include <fstream>

#include <gtest/gtest.h>

#include "mailfs/infra/logging/logger.hpp"

TEST(LoggerTest, WritesMessagesToConfiguredFileAndFiltersByLevel) {
  const auto log_path = std::filesystem::temp_directory_path() / "mailfs_logger_test.log";
  std::filesystem::remove(log_path);

  auto& logger = mailfs::infra::logging::Logger::instance();
  logger.reset_for_tests();
  logger.configure({
      mailfs::infra::logging::LogLevel::kInfo,
      log_path,
      false,
      1024 * 1024,
      2,
  });

  mailfs::infra::logging::log_debug("unit", "debug-hidden");
  mailfs::infra::logging::log_info("unit", "info-visible");
  mailfs::infra::logging::log_error("unit", "error-visible");

  std::string text;
  {
    std::ifstream input(log_path, std::ios::binary);
    ASSERT_TRUE(input.is_open());
    text.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
  }

  EXPECT_EQ(text.find("debug-hidden"), std::string::npos);
  EXPECT_NE(text.find("info-visible"), std::string::npos);
  EXPECT_NE(text.find("error-visible"), std::string::npos);
  EXPECT_NE(text.find("[INFO]"), std::string::npos);
  EXPECT_NE(text.find("[ERROR]"), std::string::npos);

  logger.reset_for_tests();
  std::filesystem::remove(log_path);
}

TEST(LoggerTest, AcceptsUtf8PathAndMessages) {
  const auto log_path = std::filesystem::temp_directory_path() / std::filesystem::u8path(u8"邮件日志.log");
  std::filesystem::remove(log_path);

  auto& logger = mailfs::infra::logging::Logger::instance();
  logger.reset_for_tests();
  logger.configure({
      mailfs::infra::logging::LogLevel::kInfo,
      log_path,
      false,
      1024 * 1024,
      2,
  });

  mailfs::infra::logging::log_info(u8"单元测试", u8"中文日志消息");
  logger.reset_for_tests();

  ASSERT_TRUE(std::filesystem::exists(log_path));
  EXPECT_GT(std::filesystem::file_size(log_path), 0U);

  std::filesystem::remove(log_path);
}

TEST(LoggerTest, RollsFileWhenMaxSizeIsExceeded) {
  const auto log_path = std::filesystem::temp_directory_path() / "mailfs_logger_rolling.log";
  const auto backup_path = std::filesystem::temp_directory_path() / "mailfs_logger_rolling.log.1";
  std::filesystem::remove(log_path);
  std::filesystem::remove(backup_path);

  auto& logger = mailfs::infra::logging::Logger::instance();
  logger.reset_for_tests();
  logger.configure({
      mailfs::infra::logging::LogLevel::kInfo,
      log_path,
      false,
      409600,
      2,
  });

  for (int i = 0; i < 12000; ++i) {
    mailfs::infra::logging::log_info("roll", "0123456789012345678901234567890123456789");
  }
  logger.reset_for_tests();

  EXPECT_TRUE(std::filesystem::exists(log_path));
  EXPECT_TRUE(std::filesystem::exists(backup_path));

  std::filesystem::remove(log_path);
  std::filesystem::remove(backup_path);
}
