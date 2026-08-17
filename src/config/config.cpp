#include "highcache/config/config.h"

#include "highcache/common/error.h"

#include <charconv>
#include <cstdint>
#include <fstream>
#include <istream>
#include <limits>
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

std::uint64_t parse_unsigned(const std::string_view value) {
  std::uint64_t parsed = 0;
  const auto [end, error] =
      std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (error != std::errc{} || end != value.data() + value.size()) {
    throw HighCacheError(ErrorCode::invalid_argument,
                         "expected an unsigned decimal integer");
  }
  return parsed;
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
  bool has_host = false;
  bool has_port = false;
  bool has_worker_threads = false;
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

    if (key == "log_level") {
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
      continue;
    }

    if (key == "host") {
      if (has_host) {
        throw_parse_error(source_name, line_number,
                          "duplicate configuration key: host");
      }
      config.host_ = value;
      has_host = true;
      continue;
    }

    if (key == "port") {
      if (has_port) {
        throw_parse_error(source_name, line_number,
                          "duplicate configuration key: port");
      }
      try {
        const auto parsed = parse_unsigned(value);
        if (parsed > std::numeric_limits<std::uint16_t>::max()) {
          throw HighCacheError(ErrorCode::invalid_argument,
                               "port exceeds uint16 range");
        }
        config.port_ = static_cast<std::uint16_t>(parsed);
      } catch (const HighCacheError &) {
        throw_parse_error(source_name, line_number,
                          "invalid port: " + std::string(value));
      }
      has_port = true;
      continue;
    }

    if (key == "worker_threads") {
      if (has_worker_threads) {
        throw_parse_error(source_name, line_number,
                          "duplicate configuration key: worker_threads");
      }
      try {
        const auto parsed = parse_unsigned(value);
        if (parsed == 0 || parsed > 1024) {
          throw HighCacheError(ErrorCode::invalid_argument,
                               "worker count outside supported range");
        }
        config.worker_threads_ = static_cast<std::size_t>(parsed);
      } catch (const HighCacheError &) {
        throw_parse_error(source_name, line_number,
                          "invalid worker_threads: " + std::string(value));
      }
      has_worker_threads = true;
      continue;
    }

    throw_parse_error(source_name, line_number,
                      "unknown configuration key: " + std::string(key));
  }

  if (input.bad()) {
    throw HighCacheError(ErrorCode::config_io,
                         "failed while reading configuration: " +
                             std::string(source_name));
  }

  return config;
}

LogLevel Config::log_level() const noexcept { return log_level_; }

const std::string &Config::host() const noexcept { return host_; }

std::uint16_t Config::port() const noexcept { return port_; }

std::size_t Config::worker_threads() const noexcept { return worker_threads_; }

} // namespace highcache
