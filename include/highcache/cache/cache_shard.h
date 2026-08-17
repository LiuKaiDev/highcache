#pragma once

#include "highcache/cache/cache.h"

#include <chrono>
#include <cstddef>
#include <mutex>
#include <string>
#include <string_view>

namespace highcache {

class CacheShard final {
public:
  explicit CacheShard(std::size_t capacity_bytes) noexcept;

  CacheShard(const CacheShard &) = delete;
  CacheShard &operator=(const CacheShard &) = delete;
  CacheShard(CacheShard &&) = delete;
  CacheShard &operator=(CacheShard &&) = delete;

  [[nodiscard]] CacheStatus
  set(std::string_view key, std::string_view value,
      std::chrono::milliseconds ttl = std::chrono::milliseconds::zero());
  [[nodiscard]] CacheStatus get(std::string_view key, std::string &output);
  [[nodiscard]] CacheStatus erase(std::string_view key);
  void tick();

  [[nodiscard]] std::size_t size() const;
  [[nodiscard]] bool empty() const;
  [[nodiscard]] std::size_t capacity_bytes() const;
  [[nodiscard]] std::size_t memory_usage_bytes() const;
  [[nodiscard]] std::size_t hit_count() const;
  [[nodiscard]] std::size_t miss_count() const;
  [[nodiscard]] std::size_t eviction_count() const;
  [[nodiscard]] std::size_t expired_count() const;

private:
  mutable std::mutex mutex_;
  Cache cache_;
};

} // namespace highcache
