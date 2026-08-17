#include "highcache/cache/cache_engine.h"
#include "highcache/common/error.h"
#include "highcache/memory/value_allocator.h"

#include <gtest/gtest.h>

#include <string>

namespace highcache {
namespace {

TEST(ValueAllocatorTest, ParsesStableBackendNames) {
  EXPECT_EQ(parse_allocator_backend("system"), AllocatorBackend::system);
  EXPECT_EQ(parse_allocator_backend("slab"), AllocatorBackend::slab);
  EXPECT_EQ(to_string(AllocatorBackend::system), "system");
  EXPECT_EQ(to_string(AllocatorBackend::slab), "slab");
  EXPECT_THROW(static_cast<void>(parse_allocator_backend("unknown")),
               HighCacheError);
}

TEST(ValueAllocatorTest, SystemBackendTracksLiveBytesWithoutSlabReservation) {
  auto allocator = make_value_allocator(AllocatorBackend::system);

  auto *const first = allocator->allocate(64);
  auto *const second = allocator->allocate(257);
  auto metrics = allocator->metrics();
  EXPECT_EQ(metrics.allocated_bytes, 321U);
  EXPECT_EQ(metrics.used_bytes, 321U);
  EXPECT_EQ(metrics.allocation_count, 2U);
  EXPECT_EQ(metrics.free_bytes, 0U);
  EXPECT_EQ(metrics.internal_fragmentation, 0U);
  EXPECT_EQ(metrics.slab_count, 0U);

  allocator->deallocate(first, 64);
  allocator->deallocate(second, 257);
  metrics = allocator->metrics();
  EXPECT_EQ(metrics.allocated_bytes, 0U);
  EXPECT_EQ(metrics.used_bytes, 0U);
  EXPECT_EQ(metrics.deallocation_count, 2U);
}

class CacheEngineAllocatorTest
    : public ::testing::TestWithParam<AllocatorBackend> {};

TEST_P(CacheEngineAllocatorTest, PreservesCacheSemanticsAndBinaryValues) {
  CacheEngine engine(1024 * 1024, 4, GetParam());
  const std::string binary("a\0b\0c", 5);

  ASSERT_EQ(engine.set("key", binary), CacheStatus::ok);
  std::string output;
  EXPECT_EQ(engine.get("key", output), CacheStatus::ok);
  EXPECT_EQ(output, binary);
  EXPECT_EQ(engine.set("key", "replacement"), CacheStatus::ok);
  EXPECT_EQ(engine.erase("key"), CacheStatus::ok);
  EXPECT_TRUE(engine.empty());
  EXPECT_EQ(engine.allocator_backend(), GetParam());

  const auto metrics = engine.allocator_metrics();
  EXPECT_EQ(metrics.used_bytes, 0U);
  EXPECT_EQ(metrics.allocation_count, 2U);
  EXPECT_EQ(metrics.deallocation_count, 2U);
}

INSTANTIATE_TEST_SUITE_P(Backends, CacheEngineAllocatorTest,
                         ::testing::Values(AllocatorBackend::system,
                                           AllocatorBackend::slab));

} // namespace
} // namespace highcache
