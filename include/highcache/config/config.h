#pragma once

#include "highcache/logging/logger.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <string>
#include <string_view>

namespace highcache {

class Config final {
public:
  Config() = default;
  explicit Config(LogLevel log_level) noexcept;

  [[nodiscard]] static Config from_file(const std::filesystem::path &path);
  [[nodiscard]] static Config
  from_stream(std::istream &input, std::string_view source_name = "<stream>");

  [[nodiscard]] LogLevel log_level() const noexcept;
  [[nodiscard]] const std::string &host() const noexcept;
  [[nodiscard]] std::uint16_t port() const noexcept;
  [[nodiscard]] std::size_t worker_threads() const noexcept;

private:
  LogLevel log_level_{LogLevel::info};
  std::string host_{"127.0.0.1"};
  std::uint16_t port_{11211};
  std::size_t worker_threads_{4};
};

} // namespace highcache
