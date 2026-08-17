#include "highcache/cache/cache.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace highcache {
namespace {

using namespace std::chrono_literals;

void tick(Cache &cache, const std::size_t count) {
  for (std::size_t tick_index = 0; tick_index < count; ++tick_index) {
    cache.tick();
  }
}

void expect_expiration_at(const std::chrono::milliseconds ttl,
                          const std::size_t expected_tick) {
  Cache cache;
  ASSERT_EQ(cache.set("key", "value", ttl), CacheStatus::ok);

  tick(cache, expected_tick - 1);
  std::string output;
  EXPECT_EQ(cache.get("key", output), CacheStatus::ok);
  EXPECT_EQ(output, "value");
  EXPECT_EQ(cache.expired_count(), 0U);

  cache.tick();
  EXPECT_EQ(cache.get("key", output), CacheStatus::not_found);
  EXPECT_EQ(cache.expired_count(), 1U);
}

TEST(CacheTtlBoundaryTest, ExpiresOneSecondTtlAtFirstTick) {
  expect_expiration_at(1s, 1);
}

TEST(CacheTtlBoundaryTest, ExpiresTwoSecondTtlAtSecondTick) {
  expect_expiration_at(2s, 2);
}

TEST(CacheTtlBoundaryTest, ExpiresSixtySecondTtlAtSixtiethTick) {
  expect_expiration_at(60s, 60);
}

TEST(CacheTtlBoundaryTest, ExpiresSixtyOneSecondTtlAfterOneFullRound) {
  expect_expiration_at(61s, 61);
}

TEST(CacheTtlBoundaryTest, ExpiresOneHundredThirtySecondTtlAfterTwoRounds) {
  expect_expiration_at(130s, 130);
}

TEST(CacheTtlBoundaryTest, RoundsPositiveSubsecondTtlUpToOneTick) {
  expect_expiration_at(1ms, 1);
  expect_expiration_at(999ms, 1);
}

TEST(CacheTtlTest, ZeroTtlCreatesPersistentEntry) {
  Cache cache;
  ASSERT_EQ(cache.set("key", "value", 0ms), CacheStatus::ok);

  tick(cache, 200);

  std::string output;
  EXPECT_EQ(cache.get("key", output), CacheStatus::ok);
  EXPECT_EQ(output, "value");
  EXPECT_EQ(cache.expired_count(), 0U);
}

TEST(CacheTtlTest, NegativeTtlPreservesValueAccountingAndExistingDeadline) {
  Cache cache;
  ASSERT_EQ(cache.set("key", "old", 2s), CacheStatus::ok);
  const auto usage_before = cache.memory_usage_bytes();

  EXPECT_EQ(cache.set("key", "new", -1ms), CacheStatus::invalid_ttl);
  EXPECT_EQ(cache.memory_usage_bytes(), usage_before);

  std::string output;
  EXPECT_EQ(cache.get("key", output), CacheStatus::ok);
  EXPECT_EQ(output, "old");
  tick(cache, 2);
  EXPECT_EQ(cache.get("key", output), CacheStatus::not_found);
  EXPECT_EQ(cache.expired_count(), 1U);
}

TEST(CacheTtlTest, NegativeTtlDoesNotChangeRecency) {
  Cache cache(4);
  ASSERT_EQ(cache.set("a", "1"), CacheStatus::ok);
  ASSERT_EQ(cache.set("b", "2"), CacheStatus::ok);

  ASSERT_EQ(cache.set("a", "3", -1s), CacheStatus::invalid_ttl);
  ASSERT_EQ(cache.set("c", "4"), CacheStatus::ok);

  std::string output;
  EXPECT_EQ(cache.get("a", output), CacheStatus::not_found);
  EXPECT_EQ(cache.get("b", output), CacheStatus::ok);
  EXPECT_EQ(cache.get("c", output), CacheStatus::ok);
}

TEST(CacheStaleTimerTest, TtlOverwriteInvalidatesOldTimer) {
  Cache cache;
  ASSERT_EQ(cache.set("a", "old", 2s), CacheStatus::ok);
  ASSERT_EQ(cache.set("a", "new", 10s), CacheStatus::ok);

  tick(cache, 2);
  std::string output;
  EXPECT_EQ(cache.get("a", output), CacheStatus::ok);
  EXPECT_EQ(output, "new");
  EXPECT_EQ(cache.expired_count(), 0U);

  tick(cache, 8);
  EXPECT_EQ(cache.get("a", output), CacheStatus::not_found);
  EXPECT_EQ(cache.expired_count(), 1U);
}

TEST(CacheStaleTimerTest, TtlToPersistentOverwriteInvalidatesTimer) {
  Cache cache;
  ASSERT_EQ(cache.set("a", "old", 2s), CacheStatus::ok);
  ASSERT_EQ(cache.set("a", "persistent"), CacheStatus::ok);

  tick(cache, 2);

  std::string output;
  EXPECT_EQ(cache.get("a", output), CacheStatus::ok);
  EXPECT_EQ(output, "persistent");
  EXPECT_EQ(cache.expired_count(), 0U);
}

TEST(CacheStaleTimerTest, PersistentToTtlOverwriteSchedulesExpiration) {
  Cache cache;
  ASSERT_EQ(cache.set("a", "persistent"), CacheStatus::ok);
  ASSERT_EQ(cache.set("a", "expiring", 2s), CacheStatus::ok);

  tick(cache, 1);
  std::string output;
  EXPECT_EQ(cache.get("a", output), CacheStatus::ok);
  EXPECT_EQ(output, "expiring");

  cache.tick();
  EXPECT_EQ(cache.get("a", output), CacheStatus::not_found);
  EXPECT_EQ(cache.expired_count(), 1U);
}

TEST(CacheStaleTimerTest, DeleteAndReinsertUsesNewGeneration) {
  Cache cache;
  ASSERT_EQ(cache.set("a", "old", 2s), CacheStatus::ok);
  ASSERT_EQ(cache.erase("a"), CacheStatus::ok);
  ASSERT_EQ(cache.set("a", "new"), CacheStatus::ok);

  tick(cache, 2);

  std::string output;
  EXPECT_EQ(cache.get("a", output), CacheStatus::ok);
  EXPECT_EQ(output, "new");
  EXPECT_EQ(cache.expired_count(), 0U);
}

TEST(CacheStaleTimerTest, LruEvictionAndReinsertUsesNewGeneration) {
  Cache cache(4);
  ASSERT_EQ(cache.set("a", "1", 2s), CacheStatus::ok);
  ASSERT_EQ(cache.set("b", "2"), CacheStatus::ok);
  ASSERT_EQ(cache.set("c", "3"), CacheStatus::ok);
  ASSERT_EQ(cache.set("a", "n"), CacheStatus::ok);

  tick(cache, 2);

  std::string output;
  EXPECT_EQ(cache.get("a", output), CacheStatus::ok);
  EXPECT_EQ(output, "n");
  EXPECT_EQ(cache.expired_count(), 0U);
  EXPECT_EQ(cache.eviction_count(), 2U);
}

TEST(CacheStaleTimerTest, OnlyLatestOfMultipleOverwritesExpires) {
  Cache cache;
  ASSERT_EQ(cache.set("a", "one", 2s), CacheStatus::ok);
  ASSERT_EQ(cache.set("a", "two", 4s), CacheStatus::ok);
  ASSERT_EQ(cache.set("a", "three", 6s), CacheStatus::ok);

  std::string output;
  tick(cache, 2);
  EXPECT_EQ(cache.get("a", output), CacheStatus::ok);
  EXPECT_EQ(output, "three");
  EXPECT_EQ(cache.expired_count(), 0U);

  tick(cache, 2);
  EXPECT_EQ(cache.get("a", output), CacheStatus::ok);
  EXPECT_EQ(cache.expired_count(), 0U);

  tick(cache, 2);
  EXPECT_EQ(cache.get("a", output), CacheStatus::not_found);
  EXPECT_EQ(cache.expired_count(), 1U);
}

TEST(CacheExpirationTest, ReleasesAccountingAndCapacityWithoutEviction) {
  Cache cache(6);
  ASSERT_EQ(cache.set("a", "1", 1s), CacheStatus::ok);
  ASSERT_EQ(cache.set("b", "2"), CacheStatus::ok);
  ASSERT_EQ(cache.set("c", "3"), CacheStatus::ok);
  ASSERT_EQ(cache.memory_usage_bytes(), 6U);

  cache.tick();

  EXPECT_EQ(cache.size(), 2U);
  EXPECT_EQ(cache.memory_usage_bytes(), 4U);
  EXPECT_EQ(cache.expired_count(), 1U);
  EXPECT_EQ(cache.eviction_count(), 0U);

  EXPECT_EQ(cache.set("d", "4"), CacheStatus::ok);
  EXPECT_EQ(cache.memory_usage_bytes(), 6U);
  EXPECT_EQ(cache.eviction_count(), 0U);
}

TEST(CacheExpirationTest, RemovingEntryPreservesRemainingLruOrder) {
  Cache cache(4);
  ASSERT_EQ(cache.set("a", "1", 1s), CacheStatus::ok);
  ASSERT_EQ(cache.set("b", "2"), CacheStatus::ok);

  std::string output;
  ASSERT_EQ(cache.get("a", output), CacheStatus::ok);
  cache.tick();
  ASSERT_EQ(cache.set("c", "3"), CacheStatus::ok);
  ASSERT_EQ(cache.set("d", "4"), CacheStatus::ok);

  EXPECT_EQ(cache.get("b", output), CacheStatus::not_found);
  EXPECT_EQ(cache.get("c", output), CacheStatus::ok);
  EXPECT_EQ(cache.get("d", output), CacheStatus::ok);
  EXPECT_EQ(cache.expired_count(), 1U);
  EXPECT_EQ(cache.eviction_count(), 1U);
}

TEST(CacheExpirationTest, StaleMissingTimerDoesNotCountExpiration) {
  Cache cache;
  ASSERT_EQ(cache.set("a", "1", 1s), CacheStatus::ok);
  ASSERT_EQ(cache.erase("a"), CacheStatus::ok);

  cache.tick();

  EXPECT_EQ(cache.expired_count(), 0U);
  EXPECT_EQ(cache.eviction_count(), 0U);
}

TEST(CacheExpirationTest, CountersRemainSemanticallySeparate) {
  Cache cache(4);
  ASSERT_EQ(cache.set("a", "1", 1s), CacheStatus::ok);
  ASSERT_EQ(cache.set("b", "2"), CacheStatus::ok);

  std::string output;
  EXPECT_EQ(cache.get("a", output), CacheStatus::ok);
  EXPECT_EQ(cache.get("missing", output), CacheStatus::not_found);
  cache.tick();
  ASSERT_EQ(cache.set("c", "3"), CacheStatus::ok);
  ASSERT_EQ(cache.set("d", "4"), CacheStatus::ok);
  ASSERT_EQ(cache.erase("c"), CacheStatus::ok);

  EXPECT_EQ(cache.hit_count(), 1U);
  EXPECT_EQ(cache.miss_count(), 1U);
  EXPECT_EQ(cache.expired_count(), 1U);
  EXPECT_EQ(cache.eviction_count(), 1U);
}

class ReferenceTtlCache final {
public:
  explicit ReferenceTtlCache(const std::size_t capacity)
      : capacity_(capacity) {}

  CacheStatus set(const std::string_view key, const std::string_view value,
                  const std::chrono::milliseconds ttl = 0ms) {
    if (key.empty()) {
      return CacheStatus::invalid_key;
    }
    if (key.size() > Cache::max_key_length) {
      return CacheStatus::key_too_large;
    }
    if (value.size() > Cache::max_value_length) {
      return CacheStatus::value_too_large;
    }
    if (ttl.count() < 0) {
      return CacheStatus::invalid_ttl;
    }

    const auto new_charge = charge(key, value);
    if (new_charge > capacity_) {
      return CacheStatus::item_too_large;
    }

    const auto deadline =
        ttl > 0ms ? std::optional<std::uint64_t>{logical_time_ + ttl_ticks(ttl)}
                  : std::nullopt;
    const auto existing = entries_.find(std::string(key));
    if (existing != entries_.end()) {
      const auto old_charge = charge(existing->first, existing->second.value);
      touch(existing->first);
      while (usage_ - old_charge > capacity_ - new_charge) {
        evict_lru();
      }
      existing->second = Entry{std::string(value), deadline};
      usage_ = usage_ - old_charge + new_charge;
      return CacheStatus::ok;
    }

    while (usage_ > capacity_ - new_charge) {
      evict_lru();
    }
    entries_.emplace(std::string(key), Entry{std::string(value), deadline});
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

    const auto entry = entries_.find(std::string(key));
    if (entry == entries_.end()) {
      ++misses_;
      return CacheStatus::not_found;
    }

    output = entry->second.value;
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

    const auto entry = entries_.find(std::string(key));
    if (entry == entries_.end()) {
      return CacheStatus::not_found;
    }

    remove_entry(entry);
    return CacheStatus::ok;
  }

  void tick() {
    ++logical_time_;
    std::vector<std::string> expired_keys;
    for (const auto &[key, entry] : entries_) {
      if (entry.deadline.has_value() && *entry.deadline <= logical_time_) {
        expired_keys.push_back(key);
      }
    }

    for (const auto &key : expired_keys) {
      remove_entry(entries_.find(key));
      ++expired_;
    }
  }

  [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
  [[nodiscard]] std::size_t usage() const noexcept { return usage_; }
  [[nodiscard]] std::size_t hits() const noexcept { return hits_; }
  [[nodiscard]] std::size_t misses() const noexcept { return misses_; }
  [[nodiscard]] std::size_t evictions() const noexcept { return evictions_; }
  [[nodiscard]] std::size_t expired() const noexcept { return expired_; }

private:
  struct Entry final {
    std::string value;
    std::optional<std::uint64_t> deadline;
  };

  using EntryMap = std::unordered_map<std::string, Entry>;

  static std::size_t charge(const std::string_view key,
                            const std::string_view value) {
    return key.size() + value.size();
  }

  static std::uint64_t ttl_ticks(const std::chrono::milliseconds ttl) {
    const auto milliseconds = static_cast<std::uint64_t>(ttl.count());
    return milliseconds / 1000 + (milliseconds % 1000 != 0);
  }

  void touch(const std::string_view key) {
    const auto position = std::find(recency_.begin(), recency_.end(), key);
    recency_.erase(position);
    recency_.insert(recency_.begin(), std::string(key));
  }

  void remove_entry(const EntryMap::iterator entry) {
    usage_ -= charge(entry->first, entry->second.value);
    const auto position =
        std::find(recency_.begin(), recency_.end(), entry->first);
    recency_.erase(position);
    entries_.erase(entry);
  }

  void evict_lru() {
    const auto key = recency_.back();
    remove_entry(entries_.find(key));
    ++evictions_;
  }

  std::size_t capacity_;
  std::size_t usage_{0};
  std::size_t hits_{0};
  std::size_t misses_{0};
  std::size_t evictions_{0};
  std::size_t expired_{0};
  std::uint64_t logical_time_{0};
  EntryMap entries_;
  std::vector<std::string> recency_;
};

TEST(CacheTtlStressTest, MatchesReferenceForOneHundredThousandMixedOperations) {
  constexpr std::size_t operation_count = 100'000;
  constexpr std::size_t capacity = 256;
  constexpr std::array<std::chrono::milliseconds, 8> ttl_values{
      1ms, 500ms, 1s, 2s, 5s, 60s, 61s, 130s};
  std::mt19937 generator(0x7711EE33U);
  std::uniform_int_distribution<int> operation_distribution(0, 6);
  std::uniform_int_distribution<int> key_distribution(0, 127);
  std::uniform_int_distribution<int> value_length_distribution(0, 64);
  std::uniform_int_distribution<std::size_t> ttl_distribution(
      0, ttl_values.size() - 1);

  Cache cache(capacity);
  ReferenceTtlCache reference(capacity);

  for (std::size_t operation = 0; operation < operation_count; ++operation) {
    const auto key = "key-" + std::to_string(key_distribution(generator));
    const auto length =
        static_cast<std::size_t>(value_length_distribution(generator));
    const std::string value(length, static_cast<char>('a' + generator() % 26));

    switch (operation_distribution(generator)) {
    case 0:
    case 1: {
      const auto ttl = ttl_values[ttl_distribution(generator)];
      EXPECT_EQ(cache.set(key, value, ttl), reference.set(key, value, ttl))
          << "operation " << operation;
      break;
    }
    case 2:
      EXPECT_EQ(cache.set(key, value), reference.set(key, value))
          << "operation " << operation;
      break;
    case 3:
      EXPECT_EQ(cache.set(key, value, -1ms), reference.set(key, value, -1ms))
          << "operation " << operation;
      break;
    case 4: {
      std::string actual_output = "unchanged";
      std::string expected_output = "unchanged";
      EXPECT_EQ(cache.get(key, actual_output),
                reference.get(key, expected_output))
          << "operation " << operation;
      EXPECT_EQ(actual_output, expected_output) << "operation " << operation;
      break;
    }
    case 5:
      EXPECT_EQ(cache.erase(key), reference.erase(key))
          << "operation " << operation;
      break;
    case 6:
      cache.tick();
      reference.tick();
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
    EXPECT_EQ(cache.expired_count(), reference.expired())
        << "operation " << operation;
  }

  for (std::size_t tick_index = 0; tick_index < 130; ++tick_index) {
    cache.tick();
    reference.tick();
  }

  for (int key_index = 0; key_index <= 127; ++key_index) {
    const auto key = "key-" + std::to_string(key_index);
    std::string actual_output = "unchanged";
    std::string expected_output = "unchanged";
    EXPECT_EQ(cache.get(key, actual_output),
              reference.get(key, expected_output));
    EXPECT_EQ(actual_output, expected_output);
  }

  EXPECT_EQ(cache.size(), reference.size());
  EXPECT_EQ(cache.memory_usage_bytes(), reference.usage());
  EXPECT_EQ(cache.hit_count(), reference.hits());
  EXPECT_EQ(cache.miss_count(), reference.misses());
  EXPECT_EQ(cache.eviction_count(), reference.evictions());
  EXPECT_EQ(cache.expired_count(), reference.expired());
}

} // namespace
} // namespace highcache
