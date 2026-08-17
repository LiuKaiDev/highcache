#include "highcache/memory/slab_allocator.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <random>
#include <utility>
#include <vector>

namespace highcache {
namespace {

TEST(SlabAllocatorTest, DefinesZeroSizeAsAnUncountedNullAllocation) {
  SlabAllocator allocator;

  EXPECT_EQ(allocator.allocate(0), nullptr);
  allocator.deallocate(nullptr, 0);

  const auto metrics = allocator.metrics();
  EXPECT_EQ(metrics.allocated_bytes, 0U);
  EXPECT_EQ(metrics.used_bytes, 0U);
  EXPECT_EQ(metrics.free_bytes, 0U);
  EXPECT_EQ(metrics.allocation_count, 0U);
  EXPECT_EQ(metrics.deallocation_count, 0U);
  EXPECT_EQ(metrics.internal_fragmentation, 0U);
  EXPECT_EQ(metrics.slab_count, 0U);
}

TEST(SlabAllocatorTest, SelectsExactClassesAtEveryBoundary) {
  constexpr std::array<std::pair<std::size_t, std::size_t>, 24> cases{{
      {1, 64},      {63, 64},     {64, 64},     {65, 128},    {127, 128},
      {128, 128},   {129, 256},   {255, 256},   {256, 256},   {257, 512},
      {511, 512},   {512, 512},   {513, 1024},  {1023, 1024}, {1024, 1024},
      {1025, 2048}, {2047, 2048}, {2048, 2048}, {2049, 4096}, {4095, 4096},
      {4096, 4096}, {4097, 8192}, {8191, 8192}, {8192, 8192},
  }};

  EXPECT_EQ(SlabAllocator::allocation_size(0), 0U);
  for (const auto &[request, expected] : cases) {
    EXPECT_EQ(SlabAllocator::allocation_size(request), expected)
        << "request=" << request;
  }
  EXPECT_EQ(SlabAllocator::allocation_size(8193), 8193U);
}

TEST(SlabAllocatorTest, AllocatesWritesAndFreesEverySizeClass) {
  SlabAllocator allocator;
  std::vector<std::pair<void *, std::size_t>> allocations;

  for (const auto size : SlabAllocator::size_classes) {
    auto *const pointer =
        static_cast<unsigned char *>(allocator.allocate(size));
    ASSERT_NE(pointer, nullptr);
    pointer[0] = 0x31;
    pointer[size - 1] = 0xA7;
    EXPECT_EQ(pointer[0], 0x31);
    EXPECT_EQ(pointer[size - 1], 0xA7);
    allocations.emplace_back(pointer, size);
  }

  auto metrics = allocator.metrics();
  EXPECT_EQ(metrics.slab_count, SlabAllocator::size_classes.size());
  EXPECT_EQ(metrics.allocated_bytes,
            SlabAllocator::size_classes.size() * SlabAllocator::slab_size);
  EXPECT_EQ(metrics.allocation_count, SlabAllocator::size_classes.size());
  EXPECT_EQ(metrics.internal_fragmentation, 0U);

  for (const auto &[pointer, size] : allocations) {
    allocator.deallocate(pointer, size);
  }

  metrics = allocator.metrics();
  EXPECT_EQ(metrics.used_bytes, 0U);
  EXPECT_EQ(metrics.free_bytes, metrics.allocated_bytes);
  EXPECT_EQ(metrics.deallocation_count, metrics.allocation_count);
  EXPECT_EQ(metrics.internal_fragmentation, 0U);
}

TEST(SlabAllocatorTest, AlignsMultipleBlocksInEveryClass) {
  SlabAllocator allocator;

  for (const auto size : SlabAllocator::size_classes) {
    std::vector<void *> pointers;
    pointers.reserve(64);
    for (std::size_t index = 0; index < 64; ++index) {
      void *const pointer = allocator.allocate(size);
      ASSERT_NE(pointer, nullptr);
      const auto address = reinterpret_cast<std::uintptr_t>(pointer);
      EXPECT_EQ(address % alignof(std::max_align_t), 0U)
          << "class=" << size << " index=" << index;
      pointers.push_back(pointer);
    }
    for (void *const pointer : pointers) {
      allocator.deallocate(pointer, size);
    }
  }
}

TEST(SlabAllocatorTest, ImmediatelyReusesAFreedBlock) {
  SlabAllocator allocator;

  void *const first = allocator.allocate(100);
  allocator.deallocate(first, 100);
  void *const second = allocator.allocate(100);

  EXPECT_EQ(second, first);
  EXPECT_EQ(allocator.metrics().slab_count, 1U);
  EXPECT_EQ(allocator.metrics().allocated_bytes, SlabAllocator::slab_size);
  allocator.deallocate(second, 100);
}

TEST(SlabAllocatorTest, GrowsBeyondOneSlabWithoutInvalidatingBlocks) {
  constexpr std::size_t object_size = 8192;
  constexpr std::size_t first_slab_capacity =
      SlabAllocator::slab_size / object_size;
  SlabAllocator allocator;
  std::vector<void *> pointers;
  pointers.reserve(first_slab_capacity + 1);

  for (std::size_t index = 0; index <= first_slab_capacity; ++index) {
    auto *const pointer =
        static_cast<unsigned char *>(allocator.allocate(object_size));
    pointer[0] = static_cast<unsigned char>(index % 251);
    pointer[object_size - 1] = static_cast<unsigned char>((index + 17) % 251);
    pointers.push_back(pointer);
  }

  EXPECT_EQ(allocator.metrics().slab_count, 2U);
  EXPECT_EQ(allocator.metrics().allocated_bytes, 2 * SlabAllocator::slab_size);
  for (std::size_t index = 0; index < pointers.size(); ++index) {
    const auto *const pointer = static_cast<unsigned char *>(pointers[index]);
    EXPECT_EQ(pointer[0], static_cast<unsigned char>(index % 251));
    EXPECT_EQ(pointer[object_size - 1],
              static_cast<unsigned char>((index + 17) % 251));
    allocator.deallocate(pointers[index], object_size);
  }

  const auto slabs_before_reuse = allocator.metrics().slab_count;
  void *const reused = allocator.allocate(object_size);
  EXPECT_EQ(allocator.metrics().slab_count, slabs_before_reuse);
  allocator.deallocate(reused, object_size);
  EXPECT_EQ(allocator.metrics().free_bytes, 2 * SlabAllocator::slab_size);
}

TEST(SlabAllocatorTest, UsesTrackedSystemFallbackForLargeObjects) {
  constexpr std::array<std::size_t, 3> sizes{8193, 16 * 1024,
                                             SlabAllocator::slab_size};
  SlabAllocator allocator;
  std::vector<std::pair<void *, std::size_t>> allocations;
  std::size_t requested_total = 0;

  for (const auto size : sizes) {
    auto *const pointer =
        static_cast<unsigned char *>(allocator.allocate(size));
    ASSERT_NE(pointer, nullptr);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(pointer) %
                  alignof(std::max_align_t),
              0U);
    pointer[0] = 0x18;
    pointer[size / 2] = 0x52;
    pointer[size - 1] = 0xA4;
    EXPECT_EQ(pointer[0], 0x18);
    EXPECT_EQ(pointer[size / 2], 0x52);
    EXPECT_EQ(pointer[size - 1], 0xA4);
    allocations.emplace_back(pointer, size);
    requested_total += size;
  }

  EXPECT_EQ(allocator.metrics().allocated_bytes, requested_total);
  EXPECT_EQ(allocator.metrics().used_bytes, requested_total);
  EXPECT_EQ(allocator.metrics().free_bytes, 0U);
  EXPECT_EQ(allocator.metrics().slab_count, 0U);

  for (const auto &[pointer, size] : allocations) {
    allocator.deallocate(pointer, size);
  }
  EXPECT_EQ(allocator.metrics().allocated_bytes, 0U);
  EXPECT_EQ(allocator.metrics().used_bytes, 0U);
  EXPECT_EQ(allocator.metrics().allocation_count, sizes.size());
  EXPECT_EQ(allocator.metrics().deallocation_count, sizes.size());
}

TEST(SlabAllocatorTest, ReportsExactMetricTransitions) {
  SlabAllocator allocator;

  void *const first = allocator.allocate(65);
  auto metrics = allocator.metrics();
  EXPECT_EQ(metrics.allocated_bytes, SlabAllocator::slab_size);
  EXPECT_EQ(metrics.used_bytes, 65U);
  EXPECT_EQ(metrics.free_bytes, SlabAllocator::slab_size - 128);
  EXPECT_EQ(metrics.allocation_count, 1U);
  EXPECT_EQ(metrics.deallocation_count, 0U);
  EXPECT_EQ(metrics.internal_fragmentation, 63U);
  EXPECT_EQ(metrics.slab_count, 1U);

  void *const second = allocator.allocate(63);
  metrics = allocator.metrics();
  EXPECT_EQ(metrics.allocated_bytes, 2 * SlabAllocator::slab_size);
  EXPECT_EQ(metrics.used_bytes, 128U);
  EXPECT_EQ(metrics.free_bytes, 2 * SlabAllocator::slab_size - 128 - 64);
  EXPECT_EQ(metrics.internal_fragmentation, 64U);

  constexpr std::size_t large_size = 9000;
  void *const large = allocator.allocate(large_size);
  metrics = allocator.metrics();
  EXPECT_EQ(metrics.allocated_bytes, 2 * SlabAllocator::slab_size + large_size);
  EXPECT_EQ(metrics.used_bytes, 128U + large_size);
  EXPECT_EQ(metrics.free_bytes, 2 * SlabAllocator::slab_size - 192);
  EXPECT_EQ(metrics.internal_fragmentation, 64U);

  allocator.deallocate(first, 65);
  allocator.deallocate(large, large_size);
  allocator.deallocate(second, 63);
  metrics = allocator.metrics();
  EXPECT_EQ(metrics.allocated_bytes, 2 * SlabAllocator::slab_size);
  EXPECT_EQ(metrics.used_bytes, 0U);
  EXPECT_EQ(metrics.free_bytes, 2 * SlabAllocator::slab_size);
  EXPECT_EQ(metrics.allocation_count, 3U);
  EXPECT_EQ(metrics.deallocation_count, 3U);
  EXPECT_EQ(metrics.internal_fragmentation, 0U);
}

TEST(SlabAllocatorStressTest, RunsOneMillionDeterministicMixedOperations) {
  struct LiveAllocation final {
    void *pointer;
    std::size_t size;
    unsigned char marker;
  };

  constexpr std::size_t operation_count = 1'000'000;
  constexpr std::size_t maximum_live = 256;
  constexpr std::array<std::size_t, 18> sizes{
      1,    64,   65,   128,  129,  256,  257,  512,  513,
      1024, 1025, 2048, 2049, 4096, 4097, 8192, 8193, 16384};
  SlabAllocator allocator;
  std::mt19937 generator(0x51AB2026U);
  std::vector<LiveAllocation> live;
  live.reserve(maximum_live);

  const auto verify = [](const LiveAllocation &allocation) {
    const auto *const bytes =
        static_cast<const unsigned char *>(allocation.pointer);
    EXPECT_EQ(bytes[0], allocation.marker);
    EXPECT_EQ(bytes[allocation.size / 2], allocation.marker);
    EXPECT_EQ(bytes[allocation.size - 1], allocation.marker);
  };

  for (std::size_t operation = 0; operation < operation_count; ++operation) {
    const bool should_allocate = live.empty() || (live.size() < maximum_live &&
                                                  (generator() % 100) < 53);
    if (should_allocate) {
      const auto size = sizes[generator() % sizes.size()];
      auto *const pointer =
          static_cast<unsigned char *>(allocator.allocate(size));
      ASSERT_NE(pointer, nullptr);
      const auto marker = static_cast<unsigned char>(
          1 + static_cast<unsigned char>(operation % 251));
      pointer[0] = marker;
      pointer[size / 2] = marker;
      pointer[size - 1] = marker;
      live.push_back({pointer, size, marker});
      continue;
    }

    const auto index = static_cast<std::size_t>(generator()) % live.size();
    verify(live[index]);
    allocator.deallocate(live[index].pointer, live[index].size);
    live[index] = live.back();
    live.pop_back();
  }

  for (const auto &allocation : live) {
    verify(allocation);
    allocator.deallocate(allocation.pointer, allocation.size);
  }

  const auto metrics = allocator.metrics();
  EXPECT_EQ(metrics.used_bytes, 0U);
  EXPECT_EQ(metrics.internal_fragmentation, 0U);
  EXPECT_EQ(metrics.allocation_count, metrics.deallocation_count);
  EXPECT_EQ(metrics.allocated_bytes,
            metrics.slab_count * SlabAllocator::slab_size);
  EXPECT_EQ(metrics.free_bytes, metrics.allocated_bytes);
}

} // namespace
} // namespace highcache
