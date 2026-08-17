#include "highcache/cache/cache_shard.h"

#include <mutex>

namespace highcache {

CacheShard::CacheShard(const std::size_t capacity_bytes) noexcept
    : cache_(capacity_bytes) {}

CacheStatus CacheShard::set(const std::string_view key,
                            const std::string_view value,
                            const std::chrono::milliseconds ttl) {
  const std::lock_guard lock(mutex_);
  return cache_.set(key, value, ttl);
}

CacheStatus CacheShard::get(const std::string_view key, std::string &output) {
  const std::lock_guard lock(mutex_);
  return cache_.get(key, output);
}

CacheStatus CacheShard::erase(const std::string_view key) {
  const std::lock_guard lock(mutex_);
  return cache_.erase(key);
}

void CacheShard::tick() {
  const std::lock_guard lock(mutex_);
  cache_.tick();
}

std::size_t CacheShard::size() const {
  const std::lock_guard lock(mutex_);
  return cache_.size();
}

bool CacheShard::empty() const {
  const std::lock_guard lock(mutex_);
  return cache_.empty();
}

std::size_t CacheShard::capacity_bytes() const {
  const std::lock_guard lock(mutex_);
  return cache_.capacity_bytes();
}

std::size_t CacheShard::memory_usage_bytes() const {
  const std::lock_guard lock(mutex_);
  return cache_.memory_usage_bytes();
}

std::size_t CacheShard::hit_count() const {
  const std::lock_guard lock(mutex_);
  return cache_.hit_count();
}

std::size_t CacheShard::miss_count() const {
  const std::lock_guard lock(mutex_);
  return cache_.miss_count();
}

std::size_t CacheShard::eviction_count() const {
  const std::lock_guard lock(mutex_);
  return cache_.eviction_count();
}

std::size_t CacheShard::expired_count() const {
  const std::lock_guard lock(mutex_);
  return cache_.expired_count();
}

} // namespace highcache
