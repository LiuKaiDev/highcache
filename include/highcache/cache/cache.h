#pragma once

#include "highcache/cache/cache_entry.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>

namespace highcache {

enum class CacheStatus {
  ok,
  not_found,
  invalid_key,
  key_too_large,
  value_too_large,
};

[[nodiscard]] std::string_view to_string(CacheStatus status) noexcept;

class Cache final {
public:
  static constexpr std::size_t max_key_length = 250;
  static constexpr std::size_t max_value_length = 1024 * 1024;

  [[nodiscard]] CacheStatus set(std::string_view key, std::string_view value);
  [[nodiscard]] CacheStatus get(std::string_view key,
                                std::string &output) const;
  [[nodiscard]] CacheStatus erase(std::string_view key);

  [[nodiscard]] std::size_t size() const noexcept;
  [[nodiscard]] bool empty() const noexcept;

private:
  [[nodiscard]] static CacheStatus validate_key(std::string_view key) noexcept;

  std::unordered_map<std::string, CacheEntry> entries_;
};

} // namespace highcache
