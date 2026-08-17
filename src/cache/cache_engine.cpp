#include "highcache/cache/cache_engine.h"

#include "highcache/common/error.h"

#include <functional>
#include <memory>

namespace highcache {

CacheEngine::CacheEngine(const std::size_t capacity_bytes,
                         const std::size_t shard_count,
                         const AllocatorBackend allocator_backend)
    : capacity_bytes_(capacity_bytes), allocator_backend_(allocator_backend) {
  if (shard_count == 0) {
    throw HighCacheError(ErrorCode::invalid_argument,
                         "cache shard count must be greater than zero");
  }

  shards_.reserve(shard_count);
  const auto base_capacity = capacity_bytes / shard_count;
  const auto remainder = capacity_bytes % shard_count;
  for (std::size_t shard_index = 0; shard_index < shard_count; ++shard_index) {
    const auto shard_capacity =
        base_capacity + static_cast<std::size_t>(shard_index < remainder);
    shards_.push_back(
        std::make_unique<CacheShard>(shard_capacity, allocator_backend_));
  }
}

CacheStatus CacheEngine::set(const std::string_view key,
                             const std::string_view value,
                             const std::chrono::milliseconds ttl) {
  return shard_for(key).set(key, value, ttl);
}

CacheStatus CacheEngine::get(const std::string_view key, std::string &output) {
  return shard_for(key).get(key, output);
}

CacheStatus CacheEngine::erase(const std::string_view key) {
  return shard_for(key).erase(key);
}

void CacheEngine::tick() {
  for (const auto &shard : shards_) {
    shard->tick();
  }
}

std::size_t CacheEngine::size() const {
  std::size_t total = 0;
  for (const auto &shard : shards_) {
    total += shard->size();
  }
  return total;
}

bool CacheEngine::empty() const {
  for (const auto &shard : shards_) {
    if (!shard->empty()) {
      return false;
    }
  }
  return true;
}

std::size_t CacheEngine::capacity_bytes() const noexcept {
  return capacity_bytes_;
}

std::size_t CacheEngine::memory_usage_bytes() const {
  std::size_t total = 0;
  for (const auto &shard : shards_) {
    total += shard->memory_usage_bytes();
  }
  return total;
}

std::size_t CacheEngine::hit_count() const {
  std::size_t total = 0;
  for (const auto &shard : shards_) {
    total += shard->hit_count();
  }
  return total;
}

std::size_t CacheEngine::miss_count() const {
  std::size_t total = 0;
  for (const auto &shard : shards_) {
    total += shard->miss_count();
  }
  return total;
}

std::size_t CacheEngine::eviction_count() const {
  std::size_t total = 0;
  for (const auto &shard : shards_) {
    total += shard->eviction_count();
  }
  return total;
}

std::size_t CacheEngine::expired_count() const {
  std::size_t total = 0;
  for (const auto &shard : shards_) {
    total += shard->expired_count();
  }
  return total;
}

SlabAllocatorMetrics CacheEngine::allocator_metrics() const {
  SlabAllocatorMetrics total;
  for (const auto &shard : shards_) {
    const auto metrics = shard->allocator_metrics();
    total.allocated_bytes += metrics.allocated_bytes;
    total.used_bytes += metrics.used_bytes;
    total.free_bytes += metrics.free_bytes;
    total.allocation_count += metrics.allocation_count;
    total.deallocation_count += metrics.deallocation_count;
    total.internal_fragmentation += metrics.internal_fragmentation;
    total.slab_count += metrics.slab_count;
  }
  return total;
}

std::size_t CacheEngine::shard_count() const noexcept { return shards_.size(); }

AllocatorBackend CacheEngine::allocator_backend() const noexcept {
  return allocator_backend_;
}

CacheShard &CacheEngine::shard_for(const std::string_view key) noexcept {
  const auto shard_index = std::hash<std::string_view>{}(key) % shards_.size();
  return *shards_[shard_index];
}

} // namespace highcache
