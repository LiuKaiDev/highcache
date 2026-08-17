#include "highcache/cache/cache.h"

#include <cassert>
#include <utility>

namespace highcache {

CacheEntry::CacheEntry(std::string value) : value_(std::move(value)) {}

const std::string &CacheEntry::value() const noexcept { return value_; }

void CacheEntry::replace_value(std::string value) noexcept {
  value_.swap(value);
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
  }

  return "unknown";
}

Cache::Cache(const std::size_t capacity_bytes) noexcept
    : capacity_bytes_(capacity_bytes) {}

CacheStatus Cache::set(const std::string_view key,
                       const std::string_view value) {
  const auto key_status = validate_key(key);
  if (key_status != CacheStatus::ok) {
    return key_status;
  }
  if (value.size() > max_value_length) {
    return CacheStatus::value_too_large;
  }

  const auto new_charge = entry_charge(key, value);
  if (new_charge > capacity_bytes_) {
    return CacheStatus::item_too_large;
  }

  auto existing = entries_.find(std::string(key));
  if (existing != entries_.end()) {
    std::string replacement(value);
    const auto old_charge =
        entry_charge(existing->first, existing->second.entry.value());

    mark_mru(existing);
    while (memory_usage_bytes_ - old_charge > capacity_bytes_ - new_charge) {
      evict_lru();
    }

    existing->second.entry.replace_value(std::move(replacement));
    memory_usage_bytes_ = memory_usage_bytes_ - old_charge + new_charge;
    return CacheStatus::ok;
  }

  std::string map_key(key);
  std::string recency_key(key);
  CacheEntry new_entry{std::string(value)};

  while (memory_usage_bytes_ > capacity_bytes_ - new_charge) {
    evict_lru();
  }

  recency_.push_front(std::move(recency_key));
  try {
    const auto inserted =
        entries_.emplace(std::move(map_key),
                         StoredEntry{std::move(new_entry), recency_.begin()});
    assert(inserted.second);
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

  output = entry->second.entry.value();
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

  memory_usage_bytes_ -=
      entry_charge(entry->first, entry->second.entry.value());
  recency_.erase(entry->second.recency_position);
  entries_.erase(entry);
  return CacheStatus::ok;
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

void Cache::evict_lru() {
  assert(!recency_.empty());
  const auto &lru_key = recency_.back();
  const auto entry = entries_.find(lru_key);
  assert(entry != entries_.end());

  memory_usage_bytes_ -=
      entry_charge(entry->first, entry->second.entry.value());
  entries_.erase(entry);
  recency_.pop_back();
  ++eviction_count_;
}

} // namespace highcache
