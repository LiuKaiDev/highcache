#pragma once

#include "highcache/memory/value_allocator.h"

#include <cstddef>
#include <string_view>

namespace highcache {

class CacheEntry final {
public:
  CacheEntry(ValueAllocator &allocator, std::string_view value);
  ~CacheEntry();

  CacheEntry(const CacheEntry &) = delete;
  CacheEntry &operator=(const CacheEntry &) = delete;
  CacheEntry(CacheEntry &&other) noexcept;
  CacheEntry &operator=(CacheEntry &&other) noexcept;

  [[nodiscard]] std::string_view value() const noexcept;
  void replace_value(CacheEntry replacement) noexcept;

private:
  ValueAllocator *allocator_;
  char *data_;
  std::size_t size_;
};

} // namespace highcache
