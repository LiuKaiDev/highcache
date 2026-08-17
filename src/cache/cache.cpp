#include "highcache/cache/cache.h"

#include <utility>

namespace highcache {

CacheEntry::CacheEntry(std::string value) : value_(std::move(value)) {}

const std::string &CacheEntry::value() const noexcept { return value_; }

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
  }

  return "unknown";
}

CacheStatus Cache::set(const std::string_view key,
                       const std::string_view value) {
  const auto key_status = validate_key(key);
  if (key_status != CacheStatus::ok) {
    return key_status;
  }
  if (value.size() > max_value_length) {
    return CacheStatus::value_too_large;
  }

  entries_.insert_or_assign(std::string(key), CacheEntry(std::string(value)));
  return CacheStatus::ok;
}

CacheStatus Cache::get(const std::string_view key, std::string &output) const {
  const auto key_status = validate_key(key);
  if (key_status != CacheStatus::ok) {
    return key_status;
  }

  const auto entry = entries_.find(std::string(key));
  if (entry == entries_.end()) {
    return CacheStatus::not_found;
  }

  output = entry->second.value();
  return CacheStatus::ok;
}

CacheStatus Cache::erase(const std::string_view key) {
  const auto key_status = validate_key(key);
  if (key_status != CacheStatus::ok) {
    return key_status;
  }

  return entries_.erase(std::string(key)) == 0 ? CacheStatus::not_found
                                               : CacheStatus::ok;
}

std::size_t Cache::size() const noexcept { return entries_.size(); }

bool Cache::empty() const noexcept { return entries_.empty(); }

CacheStatus Cache::validate_key(const std::string_view key) noexcept {
  if (key.empty()) {
    return CacheStatus::invalid_key;
  }
  if (key.size() > max_key_length) {
    return CacheStatus::key_too_large;
  }

  return CacheStatus::ok;
}

} // namespace highcache
