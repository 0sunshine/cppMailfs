#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace mailfs::infra::logging {

enum class LogLevel {
  kDebug = 0,
  kInfo = 1,
  kWarn = 2,
  kError = 3,
  kOff = 4,
};

struct LoggerConfig {
  LogLevel level = LogLevel::kInfo;
  std::filesystem::path file_path;
  bool also_stderr = true;
};

class Logger {
 public:
  static Logger& instance();

  void configure(LoggerConfig config);
  void log(LogLevel level, std::string_view component, std::string_view message);
  [[nodiscard]] bool should_log(LogLevel level) const;
  void reset_for_tests();

 private:
  Logger() = default;
};

LogLevel parse_log_level(std::string_view value);

void log_debug(std::string_view component, std::string_view message);
void log_info(std::string_view component, std::string_view message);
void log_warn(std::string_view component, std::string_view message);
void log_error(std::string_view component, std::string_view message);

}  // namespace mailfs::infra::logging
