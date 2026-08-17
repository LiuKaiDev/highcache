#pragma once

#include "highcache/cache/cache_entry.h"

#include <cstddef>
#include <list>
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
  item_too_large,
};

[[nodiscard]] std::string_view to_string(CacheStatus status) noexcept;

class Cache final {
public:
  static constexpr std::size_t max_key_length = 250;
  static constexpr std::size_t max_value_length = 1024 * 1024;
  static constexpr std::size_t default_capacity_bytes = 64 * 1024 * 1024;

  explicit Cache(std::size_t capacity_bytes = default_capacity_bytes) noexcept;

  Cache(const Cache &) = delete;
  Cache &operator=(const Cache &) = delete;
  Cache(Cache &&) = delete;
  Cache &operator=(Cache &&) = delete;

  [[nodiscard]] CacheStatus set(std::string_view key, std::string_view value);
  [[nodiscard]] CacheStatus get(std::string_view key, std::string &output);
  [[nodiscard]] CacheStatus erase(std::string_view key);

  [[nodiscard]] std::size_t size() const noexcept;
  [[nodiscard]] bool empty() const noexcept;
  [[nodiscard]] std::size_t capacity_bytes() const noexcept;
  [[nodiscard]] std::size_t memory_usage_bytes() const noexcept;
  [[nodiscard]] std::size_t hit_count() const noexcept;
  [[nodiscard]] std::size_t miss_count() const noexcept;
  [[nodiscard]] std::size_t eviction_count() const noexcept;

private:
  using RecencyList = std::list<std::string>;

  struct StoredEntry final {
    CacheEntry entry;
    RecencyList::iterator recency_position;
  };

  using EntryMap = std::unordered_map<std::string, StoredEntry>;

  [[nodiscard]] static CacheStatus validate_key(std::string_view key) noexcept;
  [[nodiscard]] static std::size_t
  entry_charge(std::string_view key, std::string_view value) noexcept;

  void mark_mru(EntryMap::iterator entry);
  void evict_lru();

  std::size_t capacity_bytes_;
  std::size_t memory_usage_bytes_{0};
  std::size_t hit_count_{0};
  std::size_t miss_count_{0};
  std::size_t eviction_count_{0};
  RecencyList recency_;
  EntryMap entries_;
};

} // namespace highcache
