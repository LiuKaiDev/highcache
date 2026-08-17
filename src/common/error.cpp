#include "highcache/common/error.h"

#include <utility>

namespace highcache {

std::string_view to_string(const ErrorCode code) noexcept {
  switch (code) {
  case ErrorCode::invalid_argument:
    return "invalid_argument";
  case ErrorCode::config_io:
    return "config_io";
  case ErrorCode::config_parse:
    return "config_parse";
  }

  return "unknown";
}

HighCacheError::HighCacheError(const ErrorCode code, std::string message)
    : std::runtime_error(std::move(message)), code_(code) {}

ErrorCode HighCacheError::code() const noexcept { return code_; }

} // namespace highcache
