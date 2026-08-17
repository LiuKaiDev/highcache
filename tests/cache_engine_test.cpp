#include "highcache/cache/cache_engine.h"

#include "highcache/common/error.h"

#include <gtest/gtest.h>

#include <atomic>
#include <barrier>
#include <chrono>
#include <cstddef>
#include <functional>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace highcache {
namespace {

using namespace std::chrono_literals;

bool is_lookup_status(const CacheStatus status) {
  return status == CacheStatus::ok || status == CacheStatus::not_found;
}

void record(const bool condition, std::atomic<bool> &success) {
  if (!condition) {
    success.store(false, std::memory_order_relaxed);
  }
}

std::vector<std::string> fixed_length_keys_for_shard(
    const std::size_t target_shard, const std::size_t count,
    const std::size_t shard_count, const std::size_t key_length) {
  std::vector<std::string> keys;
  for (std::size_t candidate = 0; keys.size() < count; ++candidate) {
    auto encoded = candidate;
    std::string key(key_length, 'a');
    for (auto &character : key) {
      character = static_cast<char>('a' + encoded % 26);
      encoded /= 26;
    }
    if (encoded != 0) {
      break;
    }
    if (std::hash<std::string_view>{}(key) % shard_count == target_shard) {
      keys.push_back(std::move(key));
    }
  }
  return keys;
}

TEST(CacheShardTest, SynchronizesCoreOperationsAndStatistics) {
  CacheShard shard(8);
  ASSERT_EQ(shard.set("key", "one", 1s), CacheStatus::ok);

  std::string output;
  EXPECT_EQ(shard.get("key", output), CacheStatus::ok);
  EXPECT_EQ(output, "one");
  EXPECT_EQ(shard.size(), 1U);
  EXPECT_FALSE(shard.empty());
  EXPECT_EQ(shard.capacity_bytes(), 8U);
  EXPECT_EQ(shard.memory_usage_bytes(), 6U);
  EXPECT_EQ(shard.hit_count(), 1U);

  shard.tick();
  EXPECT_TRUE(shard.empty());
  EXPECT_EQ(shard.expired_count(), 1U);
  EXPECT_EQ(shard.eviction_count(), 0U);
}

TEST(CacheEngineTest, ProvidesDefaultsAndRejectsZeroShards) {
  const CacheEngine engine;

  EXPECT_EQ(engine.capacity_bytes(), Cache::default_capacity_bytes);
  EXPECT_EQ(engine.shard_count(), CacheEngine::default_shard_count);
  EXPECT_TRUE(engine.empty());

  try {
    const CacheEngine invalid(1024, 0);
    static_cast<void>(invalid);
    FAIL() << "expected HighCacheError";
  } catch (const HighCacheError &error) {
    EXPECT_EQ(error.code(), ErrorCode::invalid_argument);
    EXPECT_STREQ(error.what(), "cache shard count must be greater than zero");
  }
}

class CacheEngineShardCountTest : public ::testing::TestWithParam<std::size_t> {
};

TEST_P(CacheEngineShardCountTest, RoutesSetGetDeleteOverwriteAndTtl) {
  constexpr std::size_t key_count = 256;
  CacheEngine engine(1024 * 1024, GetParam());

  for (std::size_t index = 0; index < key_count; ++index) {
    const auto key = "key-" + std::to_string(index);
    ASSERT_EQ(engine.set(key, "initial"), CacheStatus::ok);
    ASSERT_EQ(engine.set(key, "value-" + std::to_string(index)),
              CacheStatus::ok);
  }

  for (std::size_t index = 0; index < key_count; ++index) {
    const auto key = "key-" + std::to_string(index);
    std::string output;
    EXPECT_EQ(engine.get(key, output), CacheStatus::ok);
    EXPECT_EQ(output, "value-" + std::to_string(index));
  }

  for (std::size_t index = 0; index < key_count; index += 2) {
    EXPECT_EQ(engine.erase("key-" + std::to_string(index)), CacheStatus::ok);
  }

  for (std::size_t index = 1; index < key_count; index += 2) {
    ASSERT_EQ(engine.set("key-" + std::to_string(index), "ttl", 2s),
              CacheStatus::ok);
  }

  engine.tick();
  EXPECT_EQ(engine.size(), key_count / 2);
  engine.tick();
  EXPECT_TRUE(engine.empty());
  EXPECT_EQ(engine.expired_count(), key_count / 2);
}

INSTANTIATE_TEST_SUITE_P(ShardCounts, CacheEngineShardCountTest,
                         ::testing::Values(1U, 4U, 16U, 64U));

TEST(CacheEngineTest, SingleShardMatchesCacheCoreSemantics) {
  Cache core(10);
  CacheEngine engine(10, 1);

  EXPECT_EQ(engine.set("a", "1", 2s), core.set("a", "1", 2s));
  EXPECT_EQ(engine.set("b", "22"), core.set("b", "22"));
  EXPECT_EQ(engine.set("a", "new", 3s), core.set("a", "new", 3s));

  std::string core_output;
  std::string engine_output;
  EXPECT_EQ(engine.get("a", engine_output), core.get("a", core_output));
  EXPECT_EQ(engine_output, core_output);
  EXPECT_EQ(engine.erase("missing"), core.erase("missing"));

  engine.tick();
  core.tick();
  engine.tick();
  core.tick();
  engine.tick();
  core.tick();

  EXPECT_EQ(engine.size(), core.size());
  EXPECT_EQ(engine.memory_usage_bytes(), core.memory_usage_bytes());
  EXPECT_EQ(engine.hit_count(), core.hit_count());
  EXPECT_EQ(engine.miss_count(), core.miss_count());
  EXPECT_EQ(engine.eviction_count(), core.eviction_count());
  EXPECT_EQ(engine.expired_count(), core.expired_count());
}

TEST(CacheEngineCapacityTest, DistributesRemainderToLowestShardIndexes) {
  CacheEngine engine(5, 2);
  const auto shard_zero_keys = fixed_length_keys_for_shard(0, 1, 2, 2);
  const auto shard_one_keys = fixed_length_keys_for_shard(1, 1, 2, 2);
  ASSERT_EQ(shard_zero_keys.size(), 1U);
  ASSERT_EQ(shard_one_keys.size(), 1U);

  EXPECT_EQ(engine.set(shard_zero_keys.front(), "v"), CacheStatus::ok);
  EXPECT_EQ(engine.set(shard_one_keys.front(), "v"),
            CacheStatus::item_too_large);
  EXPECT_EQ(engine.memory_usage_bytes(), 3U);
  EXPECT_EQ(engine.capacity_bytes(), 5U);
}

TEST(CacheEngineCapacityTest, EvictionIsShardLocalWithoutGlobalRebalancing) {
  CacheEngine engine(8, 2);
  const auto keys = fixed_length_keys_for_shard(0, 3, 2, 1);
  ASSERT_EQ(keys.size(), 3U);

  for (const auto &key : keys) {
    ASSERT_EQ(engine.set(key, "v"), CacheStatus::ok);
  }

  EXPECT_EQ(engine.size(), 2U);
  EXPECT_EQ(engine.memory_usage_bytes(), 4U);
  EXPECT_EQ(engine.eviction_count(), 1U);
  EXPECT_LT(engine.memory_usage_bytes(), engine.capacity_bytes());
}

TEST(CacheEngineStatisticsTest, AggregatesIndependentCounterSemantics) {
  CacheEngine engine(4, 1);
  ASSERT_EQ(engine.set("a", "1", 1s), CacheStatus::ok);
  ASSERT_EQ(engine.set("b", "2"), CacheStatus::ok);

  std::string output;
  EXPECT_EQ(engine.get("a", output), CacheStatus::ok);
  EXPECT_EQ(engine.get("missing", output), CacheStatus::not_found);
  engine.tick();
  ASSERT_EQ(engine.set("c", "3"), CacheStatus::ok);
  ASSERT_EQ(engine.set("d", "4"), CacheStatus::ok);
  ASSERT_EQ(engine.erase("c"), CacheStatus::ok);

  EXPECT_EQ(engine.hit_count(), 1U);
  EXPECT_EQ(engine.miss_count(), 1U);
  EXPECT_EQ(engine.expired_count(), 1U);
  EXPECT_EQ(engine.eviction_count(), 1U);
}

TEST(CacheEngineConcurrencyTest, IndependentKeysHaveExactFinalState) {
  constexpr std::size_t thread_count = 8;
  constexpr std::size_t keys_per_thread = 1000;
  CacheEngine engine(4 * 1024 * 1024, 16);
  std::atomic<bool> success{true};
  std::barrier start(static_cast<std::ptrdiff_t>(thread_count + 1));
  std::vector<std::thread> threads;
  threads.reserve(thread_count);

  for (std::size_t thread_index = 0; thread_index < thread_count;
       ++thread_index) {
    threads.emplace_back([&, thread_index] {
      start.arrive_and_wait();
      for (std::size_t index = 0; index < keys_per_thread; ++index) {
        const auto key = "independent-" + std::to_string(thread_index) + "-" +
                         std::to_string(index);
        const auto final_value = "final-" + std::to_string(thread_index) + "-" +
                                 std::to_string(index);
        record(engine.set(key, "initial") == CacheStatus::ok, success);

        std::string output;
        record(engine.get(key, output) == CacheStatus::ok, success);
        record(output == "initial", success);
        record(engine.set(key, final_value) == CacheStatus::ok, success);
        if (index % 4 == 0) {
          record(engine.erase(key) == CacheStatus::ok, success);
          record(engine.set(key, final_value) == CacheStatus::ok, success);
        }
      }
    });
  }

  start.arrive_and_wait();
  for (auto &thread : threads) {
    thread.join();
  }

  ASSERT_TRUE(success.load(std::memory_order_relaxed));
  ASSERT_EQ(engine.size(), thread_count * keys_per_thread);
  for (std::size_t thread_index = 0; thread_index < thread_count;
       ++thread_index) {
    for (std::size_t index = 0; index < keys_per_thread; ++index) {
      const auto key = "independent-" + std::to_string(thread_index) + "-" +
                       std::to_string(index);
      std::string output;
      ASSERT_EQ(engine.get(key, output), CacheStatus::ok);
      EXPECT_EQ(output, "final-" + std::to_string(thread_index) + "-" +
                            std::to_string(index));
    }
  }
}

TEST(CacheEngineConcurrencyTest, SameKeyContentionPreservesValidResults) {
  constexpr std::size_t thread_count = 8;
  constexpr std::size_t operations_per_thread = 4000;
  CacheEngine engine(1024 * 1024, 16);
  std::atomic<bool> success{true};
  std::barrier start(static_cast<std::ptrdiff_t>(thread_count + 1));
  std::vector<std::thread> threads;
  threads.reserve(thread_count);

  for (std::size_t thread_index = 0; thread_index < thread_count;
       ++thread_index) {
    threads.emplace_back([&, thread_index] {
      start.arrive_and_wait();
      for (std::size_t operation = 0; operation < operations_per_thread;
           ++operation) {
        const auto key = "shared-" + std::to_string(operation % 32);
        switch ((operation + thread_index) % 3) {
        case 0:
          record(engine.set(key, "value-" + std::to_string(thread_index) + "-" +
                                     std::to_string(operation)) ==
                     CacheStatus::ok,
                 success);
          break;
        case 1: {
          std::string output;
          const auto status = engine.get(key, output);
          record(is_lookup_status(status), success);
          record(status != CacheStatus::ok || output.starts_with("value-"),
                 success);
          break;
        }
        case 2:
          record(is_lookup_status(engine.erase(key)), success);
          break;
        }
      }
    });
  }

  start.arrive_and_wait();
  for (auto &thread : threads) {
    thread.join();
  }

  EXPECT_TRUE(success.load(std::memory_order_relaxed));
  EXPECT_LE(engine.memory_usage_bytes(), engine.capacity_bytes());
}

TEST(CacheEngineConcurrencyTest,
     RunsOneHundredThousandFixedSeedMixedOperations) {
  constexpr std::size_t thread_count = 8;
  constexpr std::size_t operations_per_thread = 12'500;
  static_assert(thread_count * operations_per_thread == 100'000);
  CacheEngine engine(2 * 1024 * 1024, 16);
  std::atomic<bool> success{true};
  std::barrier start(static_cast<std::ptrdiff_t>(thread_count + 1));
  std::vector<std::thread> threads;
  threads.reserve(thread_count);

  for (std::size_t thread_index = 0; thread_index < thread_count;
       ++thread_index) {
    threads.emplace_back([&, thread_index] {
      std::mt19937 generator(0xC04C4000U +
                             static_cast<unsigned int>(thread_index));
      std::uniform_int_distribution<int> operation_distribution(0, 3);
      std::uniform_int_distribution<int> key_distribution(0, 511);
      start.arrive_and_wait();

      for (std::size_t operation = 0; operation < operations_per_thread;
           ++operation) {
        const auto key = "mixed-" + std::to_string(key_distribution(generator));
        const auto value = "value-" + std::to_string(thread_index) + "-" +
                           std::to_string(operation);
        switch (operation_distribution(generator)) {
        case 0:
          record(engine.set(key, value) == CacheStatus::ok, success);
          break;
        case 1: {
          const auto ttl =
              std::chrono::milliseconds{1 + static_cast<int>(generator() % 5)};
          record(engine.set(key, value, ttl) == CacheStatus::ok, success);
          break;
        }
        case 2: {
          std::string output;
          record(is_lookup_status(engine.get(key, output)), success);
          break;
        }
        case 3:
          record(is_lookup_status(engine.erase(key)), success);
          break;
        }
      }
    });
  }

  start.arrive_and_wait();
  for (auto &thread : threads) {
    thread.join();
  }

  EXPECT_TRUE(success.load(std::memory_order_relaxed));
  EXPECT_LE(engine.size(), 512U);
  EXPECT_LE(engine.memory_usage_bytes(), engine.capacity_bytes());
  engine.tick();
}

TEST(CacheEngineConcurrencyTest, TickAndMetricsAreSafeDuringMutation) {
  constexpr std::size_t worker_count = 8;
  constexpr std::size_t keys_per_worker = 64;
  constexpr std::size_t operations_per_worker = 2000;
  constexpr std::size_t tick_count = 500;
  constexpr std::size_t observer_count = 1000;
  CacheEngine engine(2 * 1024 * 1024, 16);
  std::atomic<bool> success{true};
  std::barrier start(static_cast<std::ptrdiff_t>(worker_count + 3));
  std::vector<std::thread> workers;
  workers.reserve(worker_count);

  for (std::size_t worker_index = 0; worker_index < worker_count;
       ++worker_index) {
    workers.emplace_back([&, worker_index] {
      start.arrive_and_wait();
      for (std::size_t operation = 0; operation < operations_per_worker;
           ++operation) {
        const auto key = "ttl-" + std::to_string(worker_index) + "-" +
                         std::to_string(operation % keys_per_worker);
        const auto value = "value-" + std::to_string(operation);
        record(engine.set(key, value, operation % 2 == 0 ? 1ms : 2s) ==
                   CacheStatus::ok,
               success);
        if (operation % 3 == 0) {
          std::string output;
          record(is_lookup_status(engine.get(key, output)), success);
        }
        if (operation % 5 == 0) {
          record(is_lookup_status(engine.erase(key)), success);
        }
      }

      for (std::size_t key_index = 0; key_index < keys_per_worker;
           ++key_index) {
        const auto key = "ttl-" + std::to_string(worker_index) + "-" +
                         std::to_string(key_index);
        record(engine.set(key, "final-" + std::to_string(worker_index)) ==
                   CacheStatus::ok,
               success);
      }
    });
  }

  std::thread ticker([&] {
    start.arrive_and_wait();
    for (std::size_t tick_index = 0; tick_index < tick_count; ++tick_index) {
      engine.tick();
    }
  });

  std::thread observer([&] {
    start.arrive_and_wait();
    for (std::size_t observation = 0; observation < observer_count;
         ++observation) {
      static_cast<void>(engine.size());
      record(engine.memory_usage_bytes() <= engine.capacity_bytes(), success);
      static_cast<void>(engine.hit_count());
      static_cast<void>(engine.miss_count());
      static_cast<void>(engine.eviction_count());
      static_cast<void>(engine.expired_count());
      const auto allocator_metrics = engine.allocator_metrics();
      record(allocator_metrics.used_bytes <= allocator_metrics.allocated_bytes,
             success);
    }
  });

  start.arrive_and_wait();
  for (auto &worker : workers) {
    worker.join();
  }
  ticker.join();
  observer.join();

  engine.tick();
  engine.tick();
  ASSERT_TRUE(success.load(std::memory_order_relaxed));
  ASSERT_EQ(engine.size(), worker_count * keys_per_worker);
  for (std::size_t worker_index = 0; worker_index < worker_count;
       ++worker_index) {
    for (std::size_t key_index = 0; key_index < keys_per_worker; ++key_index) {
      const auto key = "ttl-" + std::to_string(worker_index) + "-" +
                       std::to_string(key_index);
      std::string output;
      ASSERT_EQ(engine.get(key, output), CacheStatus::ok);
      EXPECT_EQ(output, "final-" + std::to_string(worker_index));
    }
  }
}

} // namespace
} // namespace highcache
