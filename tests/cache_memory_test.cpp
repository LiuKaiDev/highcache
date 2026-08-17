#include "highcache/cache/cache.h"
#include "highcache/cache/cache_engine.h"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <string>

namespace highcache {
namespace {

using namespace std::chrono_literals;

std::string binary_value(const std::size_t size, const unsigned char seed) {
  std::string value(size, '\0');
  for (std::size_t index = 0; index < size; ++index) {
    value[index] = static_cast<char>((seed + index) % 256);
  }
  if (!value.empty()) {
    value[value.size() / 2] = '\0';
  }
  return value;
}

TEST(CacheSlabValueTest, RoundTripsBoundariesAndEmbeddedNullBytes) {
  constexpr std::array<std::size_t, 9> sizes{1,    64,   65,   128, 256,
                                             1024, 4096, 8192, 8193};
  Cache cache(2 * 1024 * 1024);
  std::size_t expected_usage = 0;

  for (std::size_t index = 0; index < sizes.size(); ++index) {
    const auto key = "binary-" + std::to_string(index);
    const auto value =
        binary_value(sizes[index], static_cast<unsigned char>(index + 3));
    ASSERT_EQ(cache.set(key, value), CacheStatus::ok);
    std::string output;
    ASSERT_EQ(cache.get(key, output), CacheStatus::ok);
    EXPECT_EQ(output, value);
    expected_usage += key.size() + value.size();
  }

  EXPECT_EQ(cache.memory_usage_bytes(), expected_usage);
  EXPECT_EQ(cache.allocator_metrics().used_bytes,
            1U + 64U + 65U + 128U + 256U + 1024U + 4096U + 8192U + 8193U);
}

TEST(CacheSlabValueTest, OverwriteAndDeleteReleaseTheOldValue) {
  SlabAllocator allocator;
  Cache cache(1024, allocator);
  const auto first = binary_value(65, 11);
  const auto second = binary_value(100, 29);

  ASSERT_EQ(cache.set("key", first), CacheStatus::ok);
  EXPECT_EQ(allocator.metrics().used_bytes, first.size());
  EXPECT_EQ(allocator.metrics().internal_fragmentation, 63U);

  ASSERT_EQ(cache.set("key", second), CacheStatus::ok);
  EXPECT_EQ(allocator.metrics().used_bytes, second.size());
  EXPECT_EQ(allocator.metrics().allocation_count, 2U);
  EXPECT_EQ(allocator.metrics().deallocation_count, 1U);
  EXPECT_EQ(allocator.metrics().internal_fragmentation, 28U);

  std::string output;
  ASSERT_EQ(cache.get("key", output), CacheStatus::ok);
  EXPECT_EQ(output, second);
  ASSERT_EQ(cache.erase("key"), CacheStatus::ok);
  EXPECT_EQ(allocator.metrics().used_bytes, 0U);
  EXPECT_EQ(allocator.metrics().deallocation_count, 2U);
  EXPECT_EQ(allocator.metrics().free_bytes, SlabAllocator::slab_size);
}

TEST(CacheSlabValueTest, LruDeleteAndExpirationReturnBlocks) {
  SlabAllocator allocator;
  Cache cache(130, allocator);
  const std::string value(64, 'v');

  ASSERT_EQ(cache.set("a", value), CacheStatus::ok);
  ASSERT_EQ(cache.set("b", value), CacheStatus::ok);
  ASSERT_EQ(cache.set("c", value), CacheStatus::ok);
  EXPECT_EQ(cache.eviction_count(), 1U);
  EXPECT_EQ(allocator.metrics().used_bytes, 128U);
  EXPECT_EQ(allocator.metrics().deallocation_count, 1U);

  ASSERT_EQ(cache.erase("b"), CacheStatus::ok);
  EXPECT_EQ(allocator.metrics().used_bytes, 64U);
  ASSERT_EQ(cache.set("d", value, 1s), CacheStatus::ok);
  EXPECT_EQ(allocator.metrics().used_bytes, 128U);
  cache.tick();
  EXPECT_EQ(cache.expired_count(), 1U);
  EXPECT_EQ(allocator.metrics().used_bytes, 64U);
  ASSERT_EQ(cache.erase("c"), CacheStatus::ok);
  EXPECT_EQ(allocator.metrics().used_bytes, 0U);
}

TEST(CacheSlabValueTest, StaleTimerDoesNotFreeReplacement) {
  SlabAllocator allocator;
  Cache cache(1024, allocator);
  const auto old_value = binary_value(65, 7);
  const auto replacement = binary_value(128, 31);

  ASSERT_EQ(cache.set("key", old_value, 1s), CacheStatus::ok);
  ASSERT_EQ(cache.set("key", replacement), CacheStatus::ok);
  cache.tick();

  std::string output;
  ASSERT_EQ(cache.get("key", output), CacheStatus::ok);
  EXPECT_EQ(output, replacement);
  EXPECT_EQ(allocator.metrics().used_bytes, replacement.size());
  EXPECT_EQ(cache.expired_count(), 0U);
}

TEST(CacheSlabValueTest, LargeFallbackIsReleasedOnDelete) {
  SlabAllocator allocator;
  Cache cache(32 * 1024, allocator);
  const auto value = binary_value(8193, 43);

  ASSERT_EQ(cache.set("large", value), CacheStatus::ok);
  EXPECT_EQ(allocator.metrics().allocated_bytes, value.size());
  EXPECT_EQ(allocator.metrics().used_bytes, value.size());
  EXPECT_EQ(allocator.metrics().slab_count, 0U);
  ASSERT_EQ(cache.erase("large"), CacheStatus::ok);
  EXPECT_EQ(allocator.metrics().allocated_bytes, 0U);
  EXPECT_EQ(allocator.metrics().used_bytes, 0U);
}

TEST(CacheSlabValueTest, CacheDestructionReleasesAllLiveValues) {
  SlabAllocator allocator;
  {
    Cache cache(2 * 1024 * 1024, allocator);
    ASSERT_EQ(cache.set("small", binary_value(100, 5)), CacheStatus::ok);
    ASSERT_EQ(cache.set("large", binary_value(16 * 1024, 9)), CacheStatus::ok);
    EXPECT_GT(allocator.metrics().used_bytes, 0U);
  }

  const auto metrics = allocator.metrics();
  EXPECT_EQ(metrics.used_bytes, 0U);
  EXPECT_EQ(metrics.internal_fragmentation, 0U);
  EXPECT_EQ(metrics.allocation_count, metrics.deallocation_count);
  EXPECT_EQ(metrics.allocated_bytes, SlabAllocator::slab_size);
  EXPECT_EQ(metrics.free_bytes, SlabAllocator::slab_size);
}

TEST(CacheSlabValueTest, RepeatedMutationsReuseOneBackingSlab) {
  constexpr std::size_t iteration_count = 5000;
  SlabAllocator allocator;
  Cache cache(1024, allocator);
  const std::string first(100, 'a');
  const std::string second(101, 'b');

  for (std::size_t iteration = 0; iteration < iteration_count; ++iteration) {
    ASSERT_EQ(cache.set("key", first), CacheStatus::ok);
    ASSERT_EQ(cache.set("key", second), CacheStatus::ok);
    ASSERT_EQ(cache.erase("key"), CacheStatus::ok);
  }

  const auto metrics = allocator.metrics();
  EXPECT_EQ(metrics.slab_count, 1U);
  EXPECT_EQ(metrics.allocated_bytes, SlabAllocator::slab_size);
  EXPECT_EQ(metrics.free_bytes, SlabAllocator::slab_size);
  EXPECT_EQ(metrics.used_bytes, 0U);
  EXPECT_EQ(metrics.allocation_count, 2 * iteration_count);
  EXPECT_EQ(metrics.deallocation_count, 2 * iteration_count);
}

TEST(CacheEngineSlabValueTest, AggregatesPerShardAllocatorMetrics) {
  CacheEngine engine(1024 * 1024, 4);
  const auto first = binary_value(65, 13);
  const auto second = binary_value(8193, 17);

  ASSERT_EQ(engine.set("first", first), CacheStatus::ok);
  ASSERT_EQ(engine.set("second", second), CacheStatus::ok);
  auto metrics = engine.allocator_metrics();
  EXPECT_EQ(metrics.used_bytes, first.size() + second.size());
  EXPECT_EQ(metrics.allocation_count, 2U);

  ASSERT_EQ(engine.erase("first"), CacheStatus::ok);
  ASSERT_EQ(engine.erase("second"), CacheStatus::ok);
  metrics = engine.allocator_metrics();
  EXPECT_EQ(metrics.used_bytes, 0U);
  EXPECT_EQ(metrics.allocation_count, metrics.deallocation_count);
}

} // namespace
} // namespace highcache
