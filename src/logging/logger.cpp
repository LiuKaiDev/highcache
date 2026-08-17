#include "highcache/logging/logger.h"

#include "highcache/common/error.h"

#include <iostream>
#include <ostream>
#include <string>

namespace highcache {

std::string_view to_string(const LogLevel level) noexcept {
  switch (level) {
  case LogLevel::debug:
    return "DEBUG";
  case LogLevel::info:
    return "INFO";
  case LogLevel::warning:
    return "WARNING";
  case LogLevel::error:
    return "ERROR";
  }

  return "UNKNOWN";
}

LogLevel parse_log_level(const std::string_view value) {
  if (value == "debug") {
    return LogLevel::debug;
  }
  if (value == "info") {
    return LogLevel::info;
  }
  if (value == "warning") {
    return LogLevel::warning;
  }
  if (value == "error") {
    return LogLevel::error;
  }

  throw HighCacheError(ErrorCode::invalid_argument,
                       "unknown log level: " + std::string(value));
}

Logger::Logger(const LogLevel minimum_level)
    : Logger(std::clog, minimum_level) {}

Logger::Logger(std::ostream &output, const LogLevel minimum_level) noexcept
    : output_(output), minimum_level_(minimum_level) {}

void Logger::set_minimum_level(const LogLevel level) {
  const std::lock_guard lock(mutex_);
  minimum_level_ = level;
}

LogLevel Logger::minimum_level() const {
  const std::lock_guard lock(mutex_);
  return minimum_level_;
}

void Logger::log(const LogLevel level, const std::string_view message) {
  const std::lock_guard lock(mutex_);
  if (level < minimum_level_) {
    return;
  }

  output_ << '[' << to_string(level) << "] " << message << '\n';
}

void Logger::debug(const std::string_view message) {
  log(LogLevel::debug, message);
}

void Logger::info(const std::string_view message) {
  log(LogLevel::info, message);
}

void Logger::warning(const std::string_view message) {
  log(LogLevel::warning, message);
}

void Logger::error(const std::string_view message) {
  log(LogLevel::error, message);
}

} // namespace highcache
