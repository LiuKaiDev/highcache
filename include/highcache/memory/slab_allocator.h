#pragma once

#include "highcache/memory/value_allocator.h"

#include <array>
#include <cstddef>
#include <memory>

namespace highcache {

using SlabAllocatorMetrics = ValueAllocatorMetrics;

class SlabAllocator final : public ValueAllocator {
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

  [[nodiscard]] void *allocate(std::size_t size) override;

  // The size must exactly match the original non-zero allocation request.
  void deallocate(void *pointer, std::size_t size) noexcept override;

  [[nodiscard]] const SlabAllocatorMetrics &metrics() const noexcept override;

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
