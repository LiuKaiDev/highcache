#pragma once

#include <cstddef>
#include <memory>
#include <string_view>

namespace highcache {

struct ValueAllocatorMetrics final {
  std::size_t allocated_bytes{0};
  std::size_t used_bytes{0};
  std::size_t free_bytes{0};
  std::size_t allocation_count{0};
  std::size_t deallocation_count{0};
  std::size_t internal_fragmentation{0};
  std::size_t slab_count{0};
};

enum class AllocatorBackend {
  system,
  slab,
};

class ValueAllocator {
public:
  virtual ~ValueAllocator() = default;

  ValueAllocator(const ValueAllocator &) = delete;
  ValueAllocator &operator=(const ValueAllocator &) = delete;
  ValueAllocator(ValueAllocator &&) = delete;
  ValueAllocator &operator=(ValueAllocator &&) = delete;

  [[nodiscard]] virtual void *allocate(std::size_t size) = 0;
  virtual void deallocate(void *pointer, std::size_t size) noexcept = 0;
  [[nodiscard]] virtual const ValueAllocatorMetrics &
  metrics() const noexcept = 0;

protected:
  ValueAllocator() = default;
};

[[nodiscard]] std::unique_ptr<ValueAllocator>
make_value_allocator(AllocatorBackend backend);
[[nodiscard]] AllocatorBackend parse_allocator_backend(std::string_view value);
[[nodiscard]] std::string_view to_string(AllocatorBackend backend) noexcept;

} // namespace highcache
