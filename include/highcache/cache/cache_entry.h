#pragma once

#include "highcache/memory/slab_allocator.h"

#include <cstddef>
#include <string_view>

namespace highcache {

class CacheEntry final {
public:
  CacheEntry(SlabAllocator &allocator, std::string_view value);
  ~CacheEntry();

  CacheEntry(const CacheEntry &) = delete;
  CacheEntry &operator=(const CacheEntry &) = delete;
  CacheEntry(CacheEntry &&other) noexcept;
  CacheEntry &operator=(CacheEntry &&other) noexcept;

  [[nodiscard]] std::string_view value() const noexcept;
  void replace_value(CacheEntry replacement) noexcept;

private:
  SlabAllocator *allocator_;
  char *data_;
  std::size_t size_;
};

} // namespace highcache
