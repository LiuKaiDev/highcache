#pragma once

#include <stdexcept>
#include <string>
#include <string_view>

namespace highcache {

enum class ErrorCode {
  invalid_argument,
  config_io,
  config_parse,
  network,
};

[[nodiscard]] std::string_view to_string(ErrorCode code) noexcept;

class HighCacheError final : public std::runtime_error {
public:
  HighCacheError(ErrorCode code, std::string message);

  [[nodiscard]] ErrorCode code() const noexcept;

private:
  ErrorCode code_;
};

} // namespace highcache
