#include "highcache/cache/cache.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <random>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace highcache {
namespace {

TEST(CacheLruTest, NewEntryBecomesMostRecentlyUsed) {
  Cache cache(4);
  ASSERT_EQ(cache.set("a", "1"), CacheStatus::ok);
  ASSERT_EQ(cache.set("b", "2"), CacheStatus::ok);

  ASSERT_EQ(cache.set("c", "3"), CacheStatus::ok);

  std::string output;
  EXPECT_EQ(cache.get("a", output), CacheStatus::not_found);
  EXPECT_EQ(cache.get("b", output), CacheStatus::ok);
  EXPECT_EQ(cache.get("c", output), CacheStatus::ok);
}

TEST(CacheLruTest, CanonicalAccessPatternEvictsActualLeastRecentlyUsedEntry) {
  Cache cache(8);
  ASSERT_EQ(cache.set("a", "1"), CacheStatus::ok);
  ASSERT_EQ(cache.set("b", "2"), CacheStatus::ok);
  ASSERT_EQ(cache.set("c", "3"), CacheStatus::ok);
  ASSERT_EQ(cache.set("d", "4"), CacheStatus::ok);

  std::string output;
  ASSERT_EQ(cache.get("a", output), CacheStatus::ok);
  ASSERT_EQ(cache.set("e", "5"), CacheStatus::ok);

  EXPECT_EQ(cache.get("b", output), CacheStatus::not_found);
  EXPECT_EQ(cache.get("a", output), CacheStatus::ok);
  EXPECT_EQ(output, "1");
  EXPECT_EQ(cache.eviction_count(), 1U);
}

TEST(CacheLruTest, GetHitUpdatesRecency) {
  Cache cache(4);
  ASSERT_EQ(cache.set("a", "1"), CacheStatus::ok);
  ASSERT_EQ(cache.set("b", "2"), CacheStatus::ok);

  std::string output;
  ASSERT_EQ(cache.get("a", output), CacheStatus::ok);
  ASSERT_EQ(cache.set("c", "3"), CacheStatus::ok);

  EXPECT_EQ(cache.get("b", output), CacheStatus::not_found);
  EXPECT_EQ(cache.get("a", output), CacheStatus::ok);
}

TEST(CacheLruTest, OverwriteUpdatesRecencyWithoutDuplicatingEntry) {
  Cache cache(4);
  ASSERT_EQ(cache.set("a", "1"), CacheStatus::ok);
  ASSERT_EQ(cache.set("b", "2"), CacheStatus::ok);

  ASSERT_EQ(cache.set("a", "3"), CacheStatus::ok);
  ASSERT_EQ(cache.set("c", "4"), CacheStatus::ok);

  std::string output;
  EXPECT_EQ(cache.get("b", output), CacheStatus::not_found);
  EXPECT_EQ(cache.get("a", output), CacheStatus::ok);
  EXPECT_EQ(output, "3");
  EXPECT_EQ(cache.size(), 2U);
}

TEST(CacheLruTest, RepeatedGetsMaintainCorrectOrder) {
  Cache cache(6);
  ASSERT_EQ(cache.set("a", "1"), CacheStatus::ok);
  ASSERT_EQ(cache.set("b", "2"), CacheStatus::ok);
  ASSERT_EQ(cache.set("c", "3"), CacheStatus::ok);

  std::string output;
  ASSERT_EQ(cache.get("a", output), CacheStatus::ok);
  ASSERT_EQ(cache.get("b", output), CacheStatus::ok);
  ASSERT_EQ(cache.set("d", "4"), CacheStatus::ok);

  EXPECT_EQ(cache.get("c", output), CacheStatus::not_found);
  EXPECT_EQ(cache.get("a", output), CacheStatus::ok);
  EXPECT_EQ(cache.get("b", output), CacheStatus::ok);
}

TEST(CacheLruTest, DeleteRemovesRecencyStateWithoutCountingEviction) {
  Cache cache(4);
  ASSERT_EQ(cache.set("a", "1"), CacheStatus::ok);
  ASSERT_EQ(cache.set("b", "2"), CacheStatus::ok);

  ASSERT_EQ(cache.erase("a"), CacheStatus::ok);
  ASSERT_EQ(cache.set("c", "3"), CacheStatus::ok);

  std::string output;
  EXPECT_EQ(cache.get("b", output), CacheStatus::ok);
  EXPECT_EQ(cache.get("c", output), CacheStatus::ok);
  EXPECT_EQ(cache.eviction_count(), 0U);
}

TEST(CacheCapacityTest, ExposesConfiguredAndDefaultCapacity) {
  const Cache configured(123);
  const Cache default_cache;

  EXPECT_EQ(configured.capacity_bytes(), 123U);
  EXPECT_EQ(default_cache.capacity_bytes(), Cache::default_capacity_bytes);
}

TEST(CacheCapacityTest, InsertsBelowCapacity) {
  Cache cache(10);

  EXPECT_EQ(cache.set("a", "123"), CacheStatus::ok);
  EXPECT_EQ(cache.memory_usage_bytes(), 4U);
  EXPECT_LE(cache.memory_usage_bytes(), cache.capacity_bytes());
}

TEST(CacheCapacityTest, InsertsExactlyAtCapacity) {
  Cache cache(4);

  EXPECT_EQ(cache.set("a", "123"), CacheStatus::ok);
  EXPECT_EQ(cache.memory_usage_bytes(), cache.capacity_bytes());
}

TEST(CacheCapacityTest, EvictsOneEntryToFitInsertion) {
  Cache cache(6);
  ASSERT_EQ(cache.set("a", "11"), CacheStatus::ok);
  ASSERT_EQ(cache.set("b", "22"), CacheStatus::ok);

  ASSERT_EQ(cache.set("c", "33"), CacheStatus::ok);

  std::string output;
  EXPECT_EQ(cache.get("a", output), CacheStatus::not_found);
  EXPECT_EQ(cache.get("b", output), CacheStatus::ok);
  EXPECT_EQ(cache.get("c", output), CacheStatus::ok);
  EXPECT_EQ(cache.memory_usage_bytes(), 6U);
  EXPECT_EQ(cache.eviction_count(), 1U);
}

TEST(CacheCapacityTest, EvictsMultipleEntriesToFitInsertion) {
  Cache cache(9);
  ASSERT_EQ(cache.set("a", "11"), CacheStatus::ok);
  ASSERT_EQ(cache.set("b", "22"), CacheStatus::ok);
  ASSERT_EQ(cache.set("c", "33"), CacheStatus::ok);

  ASSERT_EQ(cache.set("d", "12345"), CacheStatus::ok);

  std::string output;
  EXPECT_EQ(cache.get("a", output), CacheStatus::not_found);
  EXPECT_EQ(cache.get("b", output), CacheStatus::not_found);
  EXPECT_EQ(cache.get("c", output), CacheStatus::ok);
  EXPECT_EQ(cache.get("d", output), CacheStatus::ok);
  EXPECT_EQ(cache.memory_usage_bytes(), 9U);
  EXPECT_EQ(cache.eviction_count(), 2U);
}

TEST(CacheCapacityTest, RejectsItemLargerThanCapacity) {
  Cache cache(4);

  EXPECT_EQ(cache.set("a", "1234"), CacheStatus::item_too_large);
  EXPECT_TRUE(cache.empty());
  EXPECT_EQ(cache.memory_usage_bytes(), 0U);
}

TEST(CacheAccountingTest, TracksInsertGrowShrinkAndDelete) {
  Cache cache(20);

  ASSERT_EQ(cache.set("aa", "123"), CacheStatus::ok);
  EXPECT_EQ(cache.memory_usage_bytes(), 5U);

  ASSERT_EQ(cache.set("aa", "123456"), CacheStatus::ok);
  EXPECT_EQ(cache.memory_usage_bytes(), 8U);

  ASSERT_EQ(cache.set("aa", "1"), CacheStatus::ok);
  EXPECT_EQ(cache.memory_usage_bytes(), 3U);

  ASSERT_EQ(cache.erase("aa"), CacheStatus::ok);
  EXPECT_EQ(cache.memory_usage_bytes(), 0U);
}

TEST(CacheAccountingTest, ValidGrowingOverwriteEvictsOtherEntriesNotItself) {
  Cache cache(8);
  ASSERT_EQ(cache.set("a", "1"), CacheStatus::ok);
  ASSERT_EQ(cache.set("b", "2"), CacheStatus::ok);
  ASSERT_EQ(cache.set("c", "3"), CacheStatus::ok);
  ASSERT_EQ(cache.set("d", "4"), CacheStatus::ok);

  ASSERT_EQ(cache.set("a", "123"), CacheStatus::ok);

  std::string output;
  EXPECT_EQ(cache.get("a", output), CacheStatus::ok);
  EXPECT_EQ(output, "123");
  EXPECT_EQ(cache.get("b", output), CacheStatus::not_found);
  EXPECT_EQ(cache.get("c", output), CacheStatus::ok);
  EXPECT_EQ(cache.get("d", output), CacheStatus::ok);
  EXPECT_EQ(cache.memory_usage_bytes(), 8U);
  EXPECT_EQ(cache.eviction_count(), 1U);
}

TEST(CacheStateTest, OversizedNewItemPreservesExistingCache) {
  Cache cache(6);
  ASSERT_EQ(cache.set("a", "1"), CacheStatus::ok);
  ASSERT_EQ(cache.set("b", "2"), CacheStatus::ok);
  const auto usage_before = cache.memory_usage_bytes();

  EXPECT_EQ(cache.set("c", "123456"), CacheStatus::item_too_large);

  std::string output;
  EXPECT_EQ(cache.get("a", output), CacheStatus::ok);
  EXPECT_EQ(cache.get("b", output), CacheStatus::ok);
  EXPECT_EQ(cache.memory_usage_bytes(), usage_before);
  EXPECT_EQ(cache.eviction_count(), 0U);
}

TEST(CacheStateTest, OversizedOverwritePreservesValueAndAccounting) {
  Cache cache(4);
  ASSERT_EQ(cache.set("a", "1"), CacheStatus::ok);
  ASSERT_EQ(cache.set("b", "2"), CacheStatus::ok);

  EXPECT_EQ(cache.set("a", "1234"), CacheStatus::item_too_large);
  EXPECT_EQ(cache.memory_usage_bytes(), 4U);
  EXPECT_EQ(cache.eviction_count(), 0U);

  std::string output;
  EXPECT_EQ(cache.get("a", output), CacheStatus::ok);
  EXPECT_EQ(output, "1");
  EXPECT_EQ(cache.get("b", output), CacheStatus::ok);
  EXPECT_EQ(output, "2");
}

TEST(CacheStateTest, OversizedOverwriteDoesNotChangeRecency) {
  Cache cache(4);
  ASSERT_EQ(cache.set("a", "1"), CacheStatus::ok);
  ASSERT_EQ(cache.set("b", "2"), CacheStatus::ok);

  ASSERT_EQ(cache.set("a", "1234"), CacheStatus::item_too_large);
  ASSERT_EQ(cache.set("c", "3"), CacheStatus::ok);

  std::string output;
  EXPECT_EQ(cache.get("a", output), CacheStatus::not_found);
  EXPECT_EQ(cache.get("b", output), CacheStatus::ok);
  EXPECT_EQ(cache.get("c", output), CacheStatus::ok);
}

TEST(CacheStateTest, InvalidSetDoesNotChangeAccountingOrRecency) {
  Cache cache(4);
  ASSERT_EQ(cache.set("a", "1"), CacheStatus::ok);
  ASSERT_EQ(cache.set("b", "2"), CacheStatus::ok);

  EXPECT_EQ(cache.set("", "invalid"), CacheStatus::invalid_key);
  EXPECT_EQ(cache.memory_usage_bytes(), 4U);

  ASSERT_EQ(cache.set("c", "3"), CacheStatus::ok);
  std::string output;
  EXPECT_EQ(cache.get("a", output), CacheStatus::not_found);
  EXPECT_EQ(cache.get("b", output), CacheStatus::ok);
}

TEST(CacheStateTest, MissingDeleteDoesNotChangeAccountingOrRecency) {
  Cache cache(4);
  ASSERT_EQ(cache.set("a", "1"), CacheStatus::ok);
  ASSERT_EQ(cache.set("b", "2"), CacheStatus::ok);

  EXPECT_EQ(cache.erase("missing"), CacheStatus::not_found);
  EXPECT_EQ(cache.memory_usage_bytes(), 4U);

  ASSERT_EQ(cache.set("c", "3"), CacheStatus::ok);
  std::string output;
  EXPECT_EQ(cache.get("a", output), CacheStatus::not_found);
  EXPECT_EQ(cache.get("b", output), CacheStatus::ok);
}

TEST(CacheCounterTest, TracksOnlyDefinedEvents) {
  Cache cache(4);
  std::string output;

  EXPECT_EQ(cache.get("", output), CacheStatus::invalid_key);
  EXPECT_EQ(cache.hit_count(), 0U);
  EXPECT_EQ(cache.miss_count(), 0U);

  EXPECT_EQ(cache.get("missing", output), CacheStatus::not_found);
  EXPECT_EQ(cache.miss_count(), 1U);

  ASSERT_EQ(cache.set("a", "1"), CacheStatus::ok);
  EXPECT_EQ(cache.get("a", output), CacheStatus::ok);
  EXPECT_EQ(cache.hit_count(), 1U);

  ASSERT_EQ(cache.set("b", "2"), CacheStatus::ok);
  ASSERT_EQ(cache.set("c", "3"), CacheStatus::ok);
  EXPECT_EQ(cache.eviction_count(), 1U);

  ASSERT_EQ(cache.erase("b"), CacheStatus::ok);
  EXPECT_EQ(cache.eviction_count(), 1U);
}

class ReferenceCache final {
public:
  explicit ReferenceCache(const std::size_t capacity) : capacity_(capacity) {}

  CacheStatus set(const std::string_view key, const std::string_view value) {
    const auto validation = validate(key, value);
    if (validation != CacheStatus::ok) {
      return validation;
    }

    const auto new_charge = charge(key, value);
    if (new_charge > capacity_) {
      return CacheStatus::item_too_large;
    }

    const auto existing = values_.find(std::string(key));
    if (existing != values_.end()) {
      const auto old_charge = charge(existing->first, existing->second);
      touch(existing->first);
      while (usage_ - old_charge > capacity_ - new_charge) {
        evict_lru();
      }
      existing->second = value;
      usage_ = usage_ - old_charge + new_charge;
      return CacheStatus::ok;
    }

    while (usage_ > capacity_ - new_charge) {
      evict_lru();
    }
    values_.emplace(key, value);
    recency_.insert(recency_.begin(), std::string(key));
    usage_ += new_charge;
    return CacheStatus::ok;
  }

  CacheStatus get(const std::string_view key, std::string &output) {
    if (key.empty()) {
      return CacheStatus::invalid_key;
    }
    if (key.size() > Cache::max_key_length) {
      return CacheStatus::key_too_large;
    }

    const auto entry = values_.find(std::string(key));
    if (entry == values_.end()) {
      ++misses_;
      return CacheStatus::not_found;
    }

    output = entry->second;
    touch(entry->first);
    ++hits_;
    return CacheStatus::ok;
  }

  CacheStatus erase(const std::string_view key) {
    if (key.empty()) {
      return CacheStatus::invalid_key;
    }
    if (key.size() > Cache::max_key_length) {
      return CacheStatus::key_too_large;
    }

    const auto entry = values_.find(std::string(key));
    if (entry == values_.end()) {
      return CacheStatus::not_found;
    }

    usage_ -= charge(entry->first, entry->second);
    const auto position = std::find(recency_.begin(), recency_.end(), key);
    recency_.erase(position);
    values_.erase(entry);
    return CacheStatus::ok;
  }

  [[nodiscard]] std::size_t size() const noexcept { return values_.size(); }
  [[nodiscard]] std::size_t usage() const noexcept { return usage_; }
  [[nodiscard]] std::size_t hits() const noexcept { return hits_; }
  [[nodiscard]] std::size_t misses() const noexcept { return misses_; }
  [[nodiscard]] std::size_t evictions() const noexcept { return evictions_; }

private:
  static CacheStatus validate(const std::string_view key,
                              const std::string_view value) {
    if (key.empty()) {
      return CacheStatus::invalid_key;
    }
    if (key.size() > Cache::max_key_length) {
      return CacheStatus::key_too_large;
    }
    if (value.size() > Cache::max_value_length) {
      return CacheStatus::value_too_large;
    }
    return CacheStatus::ok;
  }

  static std::size_t charge(const std::string_view key,
                            const std::string_view value) {
    return key.size() + value.size();
  }

  void touch(const std::string_view key) {
    const auto position = std::find(recency_.begin(), recency_.end(), key);
    recency_.erase(position);
    recency_.insert(recency_.begin(), std::string(key));
  }

  void evict_lru() {
    const auto key = recency_.back();
    const auto entry = values_.find(key);
    usage_ -= charge(entry->first, entry->second);
    values_.erase(entry);
    recency_.pop_back();
    ++evictions_;
  }

  std::size_t capacity_;
  std::size_t usage_{0};
  std::size_t hits_{0};
  std::size_t misses_{0};
  std::size_t evictions_{0};
  std::unordered_map<std::string, std::string> values_;
  std::vector<std::string> recency_;
};

TEST(CacheRandomizedTest,
     MatchesLruCapacityReferenceForOneHundredThousandOperations) {
  constexpr std::size_t operation_count = 100'000;
  constexpr std::size_t capacity = 128;
  std::mt19937 generator(0x2A11CE42U);
  std::uniform_int_distribution<int> operation_distribution(0, 2);
  std::uniform_int_distribution<int> key_distribution(0, 63);
  std::uniform_int_distribution<int> value_length_distribution(0, 180);

  Cache cache(capacity);
  ReferenceCache reference(capacity);

  for (std::size_t operation = 0; operation < operation_count; ++operation) {
    const auto key = "key-" + std::to_string(key_distribution(generator));

    switch (operation_distribution(generator)) {
    case 0: {
      const auto length =
          static_cast<std::size_t>(value_length_distribution(generator));
      const std::string value(length,
                              static_cast<char>('a' + generator() % 26));
      EXPECT_EQ(cache.set(key, value), reference.set(key, value))
          << "operation " << operation;
      break;
    }
    case 1: {
      std::string actual_output = "unchanged";
      std::string expected_output = "unchanged";
      EXPECT_EQ(cache.get(key, actual_output),
                reference.get(key, expected_output))
          << "operation " << operation;
      EXPECT_EQ(actual_output, expected_output) << "operation " << operation;
      break;
    }
    case 2:
      EXPECT_EQ(cache.erase(key), reference.erase(key))
          << "operation " << operation;
      break;
    default:
      FAIL() << "invalid generated operation";
    }

    EXPECT_EQ(cache.size(), reference.size()) << "operation " << operation;
    EXPECT_EQ(cache.memory_usage_bytes(), reference.usage())
        << "operation " << operation;
    EXPECT_LE(cache.memory_usage_bytes(), cache.capacity_bytes())
        << "operation " << operation;
    EXPECT_EQ(cache.hit_count(), reference.hits()) << "operation " << operation;
    EXPECT_EQ(cache.miss_count(), reference.misses())
        << "operation " << operation;
    EXPECT_EQ(cache.eviction_count(), reference.evictions())
        << "operation " << operation;
  }

  for (int key_index = 0; key_index <= 63; ++key_index) {
    const auto key = "key-" + std::to_string(key_index);
    std::string actual_output = "unchanged";
    std::string expected_output = "unchanged";
    EXPECT_EQ(cache.get(key, actual_output),
              reference.get(key, expected_output));
    EXPECT_EQ(actual_output, expected_output);
  }

  EXPECT_EQ(cache.memory_usage_bytes(), reference.usage());
  EXPECT_EQ(cache.hit_count(), reference.hits());
  EXPECT_EQ(cache.miss_count(), reference.misses());
  EXPECT_EQ(cache.eviction_count(), reference.evictions());
}

} // namespace
} // namespace highcache
