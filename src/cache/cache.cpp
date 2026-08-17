#include "highcache/cache/cache.h"

#include <cassert>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

namespace highcache {

CacheEntry::CacheEntry(ValueAllocator &allocator, const std::string_view value)
    : allocator_(&allocator),
      data_(static_cast<char *>(allocator.allocate(value.size()))),
      size_(value.size()) {
  if (size_ != 0) {
    std::memcpy(data_, value.data(), size_);
  }
}

CacheEntry::~CacheEntry() {
  if (allocator_ != nullptr) {
    allocator_->deallocate(data_, size_);
  }
}

CacheEntry::CacheEntry(CacheEntry &&other) noexcept
    : allocator_(other.allocator_), data_(other.data_), size_(other.size_) {
  other.allocator_ = nullptr;
  other.data_ = nullptr;
  other.size_ = 0;
}

CacheEntry &CacheEntry::operator=(CacheEntry &&other) noexcept {
  if (this == &other) {
    return *this;
  }

  if (allocator_ != nullptr) {
    allocator_->deallocate(data_, size_);
  }
  allocator_ = other.allocator_;
  data_ = other.data_;
  size_ = other.size_;
  other.allocator_ = nullptr;
  other.data_ = nullptr;
  other.size_ = 0;
  return *this;
}

std::string_view CacheEntry::value() const noexcept {
  if (size_ == 0) {
    return {};
  }
  return {data_, size_};
}

void CacheEntry::replace_value(CacheEntry replacement) noexcept {
  assert(allocator_ == replacement.allocator_);
  std::swap(data_, replacement.data_);
  std::swap(size_, replacement.size_);
}

std::string_view to_string(const CacheStatus status) noexcept {
  switch (status) {
  case CacheStatus::ok:
    return "ok";
  case CacheStatus::not_found:
    return "not_found";
  case CacheStatus::invalid_key:
    return "invalid_key";
  case CacheStatus::key_too_large:
    return "key_too_large";
  case CacheStatus::value_too_large:
    return "value_too_large";
  case CacheStatus::item_too_large:
    return "item_too_large";
  case CacheStatus::invalid_ttl:
    return "invalid_ttl";
  }

  return "unknown";
}

Cache::Cache(const std::size_t capacity_bytes)
    : capacity_bytes_(capacity_bytes), owned_allocator_(std::in_place),
      allocator_(*owned_allocator_) {}

Cache::Cache(const std::size_t capacity_bytes, ValueAllocator &allocator)
    : capacity_bytes_(capacity_bytes), owned_allocator_(std::nullopt),
      allocator_(allocator) {}

CacheStatus Cache::set(const std::string_view key, const std::string_view value,
                       const std::chrono::milliseconds ttl) {
  const auto key_status = validate_key(key);
  if (key_status != CacheStatus::ok) {
    return key_status;
  }
  if (value.size() > max_value_length) {
    return CacheStatus::value_too_large;
  }
  if (ttl.count() < 0) {
    return CacheStatus::invalid_ttl;
  }

  const auto new_charge = entry_charge(key, value);
  if (new_charge > capacity_bytes_) {
    return CacheStatus::item_too_large;
  }

  auto existing = entries_.find(std::string(key));
  if (existing != entries_.end()) {
    CacheEntry replacement{allocator_, value};
    const auto old_charge =
        entry_charge(existing->first, existing->second.entry.value());
    const auto generation = next_generation();
    if (ttl > std::chrono::milliseconds::zero()) {
      timing_wheel_.schedule(std::string(key), generation, ttl);
    }

    mark_mru(existing);
    while (memory_usage_bytes_ - old_charge > capacity_bytes_ - new_charge) {
      evict_lru();
    }

    existing->second.entry.replace_value(std::move(replacement));
    existing->second.generation = generation;
    memory_usage_bytes_ = memory_usage_bytes_ - old_charge + new_charge;
    return CacheStatus::ok;
  }

  std::string map_key(key);
  std::string recency_key(key);
  CacheEntry new_entry{allocator_, value};
  const auto generation = next_generation();
  if (ttl > std::chrono::milliseconds::zero()) {
    timing_wheel_.schedule(std::string(key), generation, ttl);
  }

  while (memory_usage_bytes_ > capacity_bytes_ - new_charge) {
    evict_lru();
  }

  recency_.push_front(std::move(recency_key));
  try {
    const auto inserted = entries_.emplace(
        std::move(map_key),
        StoredEntry{std::move(new_entry), recency_.begin(), generation});
    assert(inserted.second);
    static_cast<void>(inserted);
  } catch (...) {
    recency_.pop_front();
    throw;
  }

  memory_usage_bytes_ += new_charge;
  return CacheStatus::ok;
}

CacheStatus Cache::get(const std::string_view key, std::string &output) {
  const auto key_status = validate_key(key);
  if (key_status != CacheStatus::ok) {
    return key_status;
  }

  const auto entry = entries_.find(std::string(key));
  if (entry == entries_.end()) {
    ++miss_count_;
    return CacheStatus::not_found;
  }

  const auto value = entry->second.entry.value();
  if (value.empty()) {
    output.clear();
  } else {
    output.assign(value.data(), value.size());
  }
  mark_mru(entry);
  ++hit_count_;
  return CacheStatus::ok;
}

CacheStatus Cache::erase(const std::string_view key) {
  const auto key_status = validate_key(key);
  if (key_status != CacheStatus::ok) {
    return key_status;
  }

  const auto entry = entries_.find(std::string(key));
  if (entry == entries_.end()) {
    return CacheStatus::not_found;
  }

  remove_entry(entry);
  return CacheStatus::ok;
}

void Cache::tick() {
  for (const auto &event : timing_wheel_.tick()) {
    const auto entry = entries_.find(event.key);
    if (entry == entries_.end() ||
        entry->second.generation != event.generation) {
      continue;
    }

    remove_entry(entry);
    ++expired_count_;
  }
}

std::size_t Cache::size() const noexcept { return entries_.size(); }

bool Cache::empty() const noexcept { return entries_.empty(); }

std::size_t Cache::capacity_bytes() const noexcept { return capacity_bytes_; }

std::size_t Cache::memory_usage_bytes() const noexcept {
  return memory_usage_bytes_;
}

std::size_t Cache::hit_count() const noexcept { return hit_count_; }

std::size_t Cache::miss_count() const noexcept { return miss_count_; }

std::size_t Cache::eviction_count() const noexcept { return eviction_count_; }

std::size_t Cache::expired_count() const noexcept { return expired_count_; }

SlabAllocatorMetrics Cache::allocator_metrics() const noexcept {
  return allocator_.metrics();
}

CacheStatus Cache::validate_key(const std::string_view key) noexcept {
  if (key.empty()) {
    return CacheStatus::invalid_key;
  }
  if (key.size() > max_key_length) {
    return CacheStatus::key_too_large;
  }

  return CacheStatus::ok;
}

std::size_t Cache::entry_charge(const std::string_view key,
                                const std::string_view value) noexcept {
  return key.size() + value.size();
}

void Cache::mark_mru(const EntryMap::iterator entry) {
  recency_.splice(recency_.begin(), recency_, entry->second.recency_position);
  entry->second.recency_position = recency_.begin();
}

void Cache::remove_entry(const EntryMap::iterator entry) {
  memory_usage_bytes_ -=
      entry_charge(entry->first, entry->second.entry.value());
  recency_.erase(entry->second.recency_position);
  entries_.erase(entry);
}

void Cache::evict_lru() {
  assert(!recency_.empty());
  const auto &lru_key = recency_.back();
  const auto entry = entries_.find(lru_key);
  assert(entry != entries_.end());

  remove_entry(entry);
  ++eviction_count_;
}

std::uint64_t Cache::next_generation() {
  if (generation_source_ == std::numeric_limits<std::uint64_t>::max()) {
    throw std::overflow_error("cache entry generation exhausted");
  }

  return ++generation_source_;
}

} // namespace highcache
