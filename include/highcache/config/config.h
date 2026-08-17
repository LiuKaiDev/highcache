#pragma once

#include "highcache/logging/logger.h"

#include <filesystem>
#include <iosfwd>
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

private:
  LogLevel log_level_{LogLevel::info};
};

} // namespace highcache
