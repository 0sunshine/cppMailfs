#include "mailfs/infra/logging/logger.hpp"
#include "mailfs/infra/platform/utf8.hpp"

#include <algorithm>
#include <cctype>
#include <memory>
#include <mutex>
#include <stdexcept>

#include <log4cplus/consoleappender.h>
#include <log4cplus/fileappender.h>
#include <log4cplus/initializer.h>
#include <log4cplus/layout.h>
#include <log4cplus/logger.h>
#include <log4cplus/loggingmacros.h>

namespace mailfs::infra::logging {

namespace {

struct LoggerState {
  LoggerConfig config;
  std::unique_ptr<log4cplus::Initializer> initializer;
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

log4cplus::LogLevel native_level(LogLevel level) {
  switch (level) {
    case LogLevel::kDebug:
      return log4cplus::DEBUG_LOG_LEVEL;
    case LogLevel::kInfo:
      return log4cplus::INFO_LOG_LEVEL;
    case LogLevel::kWarn:
      return log4cplus::WARN_LOG_LEVEL;
    case LogLevel::kError:
      return log4cplus::ERROR_LOG_LEVEL;
    case LogLevel::kOff:
      return log4cplus::OFF_LOG_LEVEL;
  }
  return log4cplus::OFF_LOG_LEVEL;
}

log4cplus::tstring to_tstring(std::string_view value) {
#ifdef UNICODE
  return mailfs::infra::platform::utf8_to_wide(value);
#else
  return std::string(value);
#endif
}

}  // namespace

Logger& Logger::instance() {
  static Logger logger;
  return logger;
}

void Logger::configure(LoggerConfig config) {
  auto& current = state();
  std::lock_guard<std::mutex> lock(current.mutex);

  if (!current.initializer) {
    current.initializer = std::make_unique<log4cplus::Initializer>();
  }

  auto root = log4cplus::Logger::getRoot();
  root.removeAllAppenders();
  root.setLogLevel(native_level(config.level));

  const auto pattern = LOG4CPLUS_TEXT("%D{%Y-%m-%dT%H:%M:%S,%q} [%p] [%c] %m%n");
  if (!config.file_path.empty()) {
    if (!config.file_path.parent_path().empty()) {
      std::filesystem::create_directories(config.file_path.parent_path());
    }

    auto appender = log4cplus::SharedAppenderPtr(new log4cplus::RollingFileAppender(
        to_tstring(config.file_path.u8string()),
        static_cast<long>(config.max_file_size),
        std::max(0, config.max_backup_files),
        true,
        true));
    appender->setName(LOG4CPLUS_TEXT("mailfs.file"));
    appender->setLayout(std::make_unique<log4cplus::PatternLayout>(pattern));
    root.addAppender(appender);
  }

  if (config.also_stderr || config.file_path.empty()) {
    auto appender = log4cplus::SharedAppenderPtr(new log4cplus::ConsoleAppender(true));
    appender->setName(LOG4CPLUS_TEXT("mailfs.stderr"));
    appender->setLayout(std::make_unique<log4cplus::PatternLayout>(pattern));
    root.addAppender(appender);
  }

  current.config = std::move(config);
}

void Logger::log(LogLevel level, std::string_view component, std::string_view message) {
  auto& current = state();
  std::lock_guard<std::mutex> lock(current.mutex);
  if (level < current.config.level || current.config.level == LogLevel::kOff) {
    return;
  }

  auto logger = log4cplus::Logger::getInstance(to_tstring(component));
  const auto text = to_tstring(message);
  switch (level) {
    case LogLevel::kDebug:
      LOG4CPLUS_DEBUG(logger, text);
      break;
    case LogLevel::kInfo:
      LOG4CPLUS_INFO(logger, text);
      break;
    case LogLevel::kWarn:
      LOG4CPLUS_WARN(logger, text);
      break;
    case LogLevel::kError:
      LOG4CPLUS_ERROR(logger, text);
      break;
    case LogLevel::kOff:
      break;
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
  log4cplus::Logger::getRoot().removeAllAppenders();
  current.initializer.reset();
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
