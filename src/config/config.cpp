#include "highcache/config/config.h"

#include "highcache/common/error.h"

#include <fstream>
#include <istream>
#include <string>

namespace highcache {
namespace {

std::string_view trim(const std::string_view value) {
  constexpr std::string_view whitespace{" \t\r\n"};
  const auto first = value.find_first_not_of(whitespace);
  if (first == std::string_view::npos) {
    return {};
  }

  const auto last = value.find_last_not_of(whitespace);
  return value.substr(first, last - first + 1);
}

[[noreturn]] void throw_parse_error(const std::string_view source_name,
                                    const std::size_t line_number,
                                    const std::string_view detail) {
  throw HighCacheError(ErrorCode::config_parse,
                       std::string(source_name) + ":" +
                           std::to_string(line_number) + ": " +
                           std::string(detail));
}

} // namespace

Config::Config(const LogLevel log_level) noexcept : log_level_(log_level) {}

Config Config::from_file(const std::filesystem::path &path) {
  std::ifstream input(path);
  if (!input) {
    throw HighCacheError(ErrorCode::config_io,
                         "unable to open configuration file: " + path.string());
  }

  return from_stream(input, path.string());
}

Config Config::from_stream(std::istream &input,
                           const std::string_view source_name) {
  Config config;
  bool has_log_level = false;
  std::string line;
  std::size_t line_number = 0;

  while (std::getline(input, line)) {
    ++line_number;
    const auto content = trim(line);
    if (content.empty() || content.front() == '#') {
      continue;
    }

    const auto delimiter = content.find('=');
    if (delimiter == std::string_view::npos) {
      throw_parse_error(source_name, line_number, "expected key=value");
    }

    const auto key = trim(content.substr(0, delimiter));
    const auto value = trim(content.substr(delimiter + 1));
    if (key.empty() || value.empty()) {
      throw_parse_error(source_name, line_number,
                        "key and value must not be empty");
    }

    if (key != "log_level") {
      throw_parse_error(source_name, line_number,
                        "unknown configuration key: " + std::string(key));
    }
    if (has_log_level) {
      throw_parse_error(source_name, line_number,
                        "duplicate configuration key: log_level");
    }

    try {
      config.log_level_ = parse_log_level(value);
    } catch (const HighCacheError &) {
      throw_parse_error(source_name, line_number,
                        "invalid log_level: " + std::string(value));
    }
    has_log_level = true;
  }

  if (input.bad()) {
    throw HighCacheError(ErrorCode::config_io,
                         "failed while reading configuration: " +
                             std::string(source_name));
  }

  return config;
}

LogLevel Config::log_level() const noexcept { return log_level_; }

} // namespace highcache
