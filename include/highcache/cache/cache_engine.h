#pragma once

#include "highcache/cache/cache_shard.h"

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace highcache {

class CacheEngine final {
public:
  static constexpr std::size_t default_shard_count = 64;

  explicit CacheEngine(
      std::size_t capacity_bytes = Cache::default_capacity_bytes,
      std::size_t shard_count = default_shard_count);

  CacheEngine(const CacheEngine &) = delete;
  CacheEngine &operator=(const CacheEngine &) = delete;
  CacheEngine(CacheEngine &&) = delete;
  CacheEngine &operator=(CacheEngine &&) = delete;

  [[nodiscard]] CacheStatus
  set(std::string_view key, std::string_view value,
      std::chrono::milliseconds ttl = std::chrono::milliseconds::zero());
  [[nodiscard]] CacheStatus get(std::string_view key, std::string &output);
  [[nodiscard]] CacheStatus erase(std::string_view key);
  void tick();

  [[nodiscard]] std::size_t size() const;
  [[nodiscard]] bool empty() const;
  [[nodiscard]] std::size_t capacity_bytes() const noexcept;
  [[nodiscard]] std::size_t memory_usage_bytes() const;
  [[nodiscard]] std::size_t hit_count() const;
  [[nodiscard]] std::size_t miss_count() const;
  [[nodiscard]] std::size_t eviction_count() const;
  [[nodiscard]] std::size_t expired_count() const;
  [[nodiscard]] SlabAllocatorMetrics allocator_metrics() const;
  [[nodiscard]] std::size_t shard_count() const noexcept;

private:
  [[nodiscard]] CacheShard &shard_for(std::string_view key) noexcept;

  std::size_t capacity_bytes_;
  std::vector<std::unique_ptr<CacheShard>> shards_;
};

} // namespace highcache
