#pragma once

#include <array>
#include <cstddef>
#include <memory>

namespace highcache {

struct SlabAllocatorMetrics final {
  std::size_t allocated_bytes{0};
  std::size_t used_bytes{0};
  std::size_t free_bytes{0};
  std::size_t allocation_count{0};
  std::size_t deallocation_count{0};
  std::size_t internal_fragmentation{0};
  std::size_t slab_count{0};
};

class SlabAllocator final {
public:
  static constexpr std::size_t slab_size = 1024 * 1024;
  static constexpr std::array<std::size_t, 8> size_classes{
      64, 128, 256, 512, 1024, 2048, 4096, 8192};

  SlabAllocator();
  ~SlabAllocator();

  SlabAllocator(const SlabAllocator &) = delete;
  SlabAllocator &operator=(const SlabAllocator &) = delete;
  SlabAllocator(SlabAllocator &&) = delete;
  SlabAllocator &operator=(SlabAllocator &&) = delete;

  [[nodiscard]] void *allocate(std::size_t size);

  // The size must exactly match the original non-zero allocation request.
  void deallocate(void *pointer, std::size_t size) noexcept;

  [[nodiscard]] const SlabAllocatorMetrics &metrics() const noexcept;

  // Returns zero for zero-size requests, the selected class for slab requests,
  // and the request itself for large-object fallback allocations.
  [[nodiscard]] static std::size_t
  allocation_size(std::size_t requested_size) noexcept;

private:
  class SlabClass;

  [[nodiscard]] static std::size_t
  class_index(std::size_t requested_size) noexcept;

  std::array<std::unique_ptr<SlabClass>, size_classes.size()> classes_;
  SlabAllocatorMetrics metrics_;
};

} // namespace highcache
