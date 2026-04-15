#include "mailfs/infra/logging/logger.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>

#include <android/log.h>

namespace mailfs::infra::logging {

namespace {

struct LoggerState {
  LoggerConfig config;
  mutable std::mutex mutex;
};

LoggerState& state() {
  static LoggerState value;
  return value;
}

std::string lower_copy(std::string_view value) {
  std::string lowered(value);
  std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return lowered;
}

int android_priority(LogLevel level) {
  switch (level) {
    case LogLevel::kDebug:
      return ANDROID_LOG_DEBUG;
    case LogLevel::kInfo:
      return ANDROID_LOG_INFO;
    case LogLevel::kWarn:
      return ANDROID_LOG_WARN;
    case LogLevel::kError:
      return ANDROID_LOG_ERROR;
    case LogLevel::kOff:
      return ANDROID_LOG_SILENT;
  }
  return ANDROID_LOG_SILENT;
}

const char* level_name(LogLevel level) {
  switch (level) {
    case LogLevel::kDebug:
      return "DEBUG";
    case LogLevel::kInfo:
      return "INFO";
    case LogLevel::kWarn:
      return "WARN";
    case LogLevel::kError:
      return "ERROR";
    case LogLevel::kOff:
      return "OFF";
  }
  return "OFF";
}

}  // namespace

Logger& Logger::instance() {
  static Logger logger;
  return logger;
}

void Logger::configure(LoggerConfig config) {
  auto& current = state();
  std::lock_guard<std::mutex> lock(current.mutex);
  current.config = std::move(config);
  if (!current.config.file_path.empty() && current.config.file_path.has_parent_path()) {
    std::filesystem::create_directories(current.config.file_path.parent_path());
  }
}

void Logger::log(LogLevel level, std::string_view component, std::string_view message) {
  auto& current = state();
  std::lock_guard<std::mutex> lock(current.mutex);
  if (level < current.config.level || current.config.level == LogLevel::kOff) {
    return;
  }

  const auto text = "[" + std::string(component) + "] " + std::string(message);
  __android_log_write(android_priority(level), "mailfs", text.c_str());

  if (!current.config.file_path.empty()) {
    std::ofstream output(current.config.file_path, std::ios::binary | std::ios::app);
    if (output) {
      output << level_name(level) << ' ' << text << '\n';
    }
  }
}

bool Logger::should_log(LogLevel level) const {
  auto& current = state();
  std::lock_guard<std::mutex> lock(current.mutex);
  return current.config.level != LogLevel::kOff && level >= current.config.level;
}

void Logger::reset_for_tests() {
  auto& current = state();
  std::lock_guard<std::mutex> lock(current.mutex);
  current.config = LoggerConfig{};
}

LogLevel parse_log_level(std::string_view value) {
  const auto lowered = lower_copy(value);
  if (lowered == "debug") {
    return LogLevel::kDebug;
  }
  if (lowered == "info") {
    return LogLevel::kInfo;
  }
  if (lowered == "warn" || lowered == "warning") {
    return LogLevel::kWarn;
  }
  if (lowered == "error") {
    return LogLevel::kError;
  }
  if (lowered == "off") {
    return LogLevel::kOff;
  }
  throw std::runtime_error("unsupported log level: " + std::string(value));
}

void log_debug(std::string_view component, std::string_view message) {
  Logger::instance().log(LogLevel::kDebug, component, message);
}

void log_info(std::string_view component, std::string_view message) {
  Logger::instance().log(LogLevel::kInfo, component, message);
}

void log_warn(std::string_view component, std::string_view message) {
  Logger::instance().log(LogLevel::kWarn, component, message);
}

void log_error(std::string_view component, std::string_view message) {
  Logger::instance().log(LogLevel::kError, component, message);
}

}  // namespace mailfs::infra::logging
