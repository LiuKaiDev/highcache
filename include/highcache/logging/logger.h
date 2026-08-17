#pragma once

#include <iosfwd>
#include <mutex>
#include <string_view>

namespace highcache {

enum class LogLevel {
  debug,
  info,
  warning,
  error,
};

[[nodiscard]] std::string_view to_string(LogLevel level) noexcept;
[[nodiscard]] LogLevel parse_log_level(std::string_view value);

class Logger final {
public:
  explicit Logger(LogLevel minimum_level = LogLevel::info);
  Logger(std::ostream &output,
         LogLevel minimum_level = LogLevel::info) noexcept;

  Logger(const Logger &) = delete;
  Logger &operator=(const Logger &) = delete;

  void set_minimum_level(LogLevel level);
  [[nodiscard]] LogLevel minimum_level() const;

  void log(LogLevel level, std::string_view message);
  void debug(std::string_view message);
  void info(std::string_view message);
  void warning(std::string_view message);
  void error(std::string_view message);

private:
  std::ostream &output_;
  LogLevel minimum_level_;
  mutable std::mutex mutex_;
};

} // namespace highcache
