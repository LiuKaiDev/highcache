#include "highcache/cache/cache.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <random>
#include <string>
#include <unordered_map>

namespace highcache {
namespace {

TEST(CacheEntryTest, OwnsItsValue) {
  std::string source = "original";
  const CacheEntry entry(source);

  source = "changed";

  EXPECT_EQ(entry.value(), "original");
}

TEST(CacheStatusTest, ProvidesStableNames) {
  EXPECT_EQ(to_string(CacheStatus::ok), "ok");
  EXPECT_EQ(to_string(CacheStatus::not_found), "not_found");
  EXPECT_EQ(to_string(CacheStatus::invalid_key), "invalid_key");
  EXPECT_EQ(to_string(CacheStatus::key_too_large), "key_too_large");
  EXPECT_EQ(to_string(CacheStatus::value_too_large), "value_too_large");
  EXPECT_EQ(to_string(CacheStatus::item_too_large), "item_too_large");
  EXPECT_EQ(to_string(CacheStatus::invalid_ttl), "invalid_ttl");
}

TEST(CacheTest, SetsThenGetsValue) {
  Cache cache;
  std::string output;

  EXPECT_EQ(cache.set("key", "value"), CacheStatus::ok);
  EXPECT_EQ(cache.get("key", output), CacheStatus::ok);
  EXPECT_EQ(output, "value");
  EXPECT_EQ(cache.size(), 1U);
}

TEST(CacheTest, DeletesExistingKey) {
  Cache cache;
  ASSERT_EQ(cache.set("key", "value"), CacheStatus::ok);

  EXPECT_EQ(cache.erase("key"), CacheStatus::ok);
  EXPECT_TRUE(cache.empty());

  std::string output;
  EXPECT_EQ(cache.get("key", output), CacheStatus::not_found);
}

TEST(CacheTest, ReportsMissingGetWithoutChangingOutput) {
  Cache cache;
  std::string output = "unchanged";

  EXPECT_EQ(cache.get("missing", output), CacheStatus::not_found);
  EXPECT_EQ(output, "unchanged");
}

TEST(CacheTest, ReportsMissingDelete) {
  Cache cache;

  EXPECT_EQ(cache.erase("missing"), CacheStatus::not_found);
}

TEST(CacheTest, OverwritesExistingValue) {
  Cache cache;
  ASSERT_EQ(cache.set("key", "first"), CacheStatus::ok);

  EXPECT_EQ(cache.set("key", "second"), CacheStatus::ok);

  std::string output;
  EXPECT_EQ(cache.get("key", output), CacheStatus::ok);
  EXPECT_EQ(output, "second");
  EXPECT_EQ(cache.size(), 1U);
}

TEST(CacheTest, SupportsMultipleOverwrites) {
  Cache cache;

  ASSERT_EQ(cache.set("key", "one"), CacheStatus::ok);
  ASSERT_EQ(cache.set("key", "two"), CacheStatus::ok);
  ASSERT_EQ(cache.set("key", "three"), CacheStatus::ok);

  std::string output;
  EXPECT_EQ(cache.get("key", output), CacheStatus::ok);
  EXPECT_EQ(output, "three");
}

TEST(CacheTest, DeletesThenReinserts) {
  Cache cache;
  ASSERT_EQ(cache.set("key", "first"), CacheStatus::ok);
  ASSERT_EQ(cache.erase("key"), CacheStatus::ok);

  EXPECT_EQ(cache.set("key", "second"), CacheStatus::ok);

  std::string output;
  EXPECT_EQ(cache.get("key", output), CacheStatus::ok);
  EXPECT_EQ(output, "second");
}

TEST(CacheTest, SupportsEmptyValue) {
  Cache cache;
  std::string output = "not empty";

  EXPECT_EQ(cache.set("key", ""), CacheStatus::ok);
  EXPECT_EQ(cache.get("key", output), CacheStatus::ok);
  EXPECT_TRUE(output.empty());
}

TEST(CacheTest, RejectsEmptyKeyForEveryOperation) {
  Cache cache;
  std::string output = "unchanged";

  EXPECT_EQ(cache.set("", "value"), CacheStatus::invalid_key);
  EXPECT_EQ(cache.get("", output), CacheStatus::invalid_key);
  EXPECT_EQ(cache.erase(""), CacheStatus::invalid_key);
  EXPECT_EQ(output, "unchanged");
  EXPECT_TRUE(cache.empty());
}

TEST(CacheTest, AcceptsKeyAtMaximumLength) {
  Cache cache;
  const std::string key(Cache::max_key_length, 'k');

  EXPECT_EQ(cache.set(key, "value"), CacheStatus::ok);

  std::string output;
  EXPECT_EQ(cache.get(key, output), CacheStatus::ok);
  EXPECT_EQ(output, "value");
}

TEST(CacheTest, RejectsKeyAboveMaximumLength) {
  Cache cache;
  const std::string key(Cache::max_key_length + 1, 'k');
  std::string output = "unchanged";

  EXPECT_EQ(cache.set(key, "value"), CacheStatus::key_too_large);
  EXPECT_EQ(cache.get(key, output), CacheStatus::key_too_large);
  EXPECT_EQ(cache.erase(key), CacheStatus::key_too_large);
  EXPECT_EQ(output, "unchanged");
  EXPECT_TRUE(cache.empty());
}

TEST(CacheTest, AcceptsValueAtMaximumLength) {
  Cache cache;
  const std::string value(Cache::max_value_length, 'v');

  EXPECT_EQ(cache.set("key", value), CacheStatus::ok);

  std::string output;
  EXPECT_EQ(cache.get("key", output), CacheStatus::ok);
  EXPECT_EQ(output, value);
}

TEST(CacheTest, RejectsValueAboveMaximumLength) {
  Cache cache;
  const std::string value(Cache::max_value_length + 1, 'v');

  EXPECT_EQ(cache.set("key", value), CacheStatus::value_too_large);
  EXPECT_TRUE(cache.empty());
}

TEST(CacheTest, KeepsIndependentKeys) {
  Cache cache;
  ASSERT_EQ(cache.set("alpha", "one"), CacheStatus::ok);
  ASSERT_EQ(cache.set("beta", "two"), CacheStatus::ok);
  ASSERT_EQ(cache.set("gamma", "three"), CacheStatus::ok);

  std::string output;
  EXPECT_EQ(cache.get("alpha", output), CacheStatus::ok);
  EXPECT_EQ(output, "one");
  EXPECT_EQ(cache.get("beta", output), CacheStatus::ok);
  EXPECT_EQ(output, "two");
  EXPECT_EQ(cache.get("gamma", output), CacheStatus::ok);
  EXPECT_EQ(output, "three");
  EXPECT_EQ(cache.size(), 3U);
}

TEST(CacheTest, DeletingOneKeyDoesNotAffectOthers) {
  Cache cache;
  ASSERT_EQ(cache.set("alpha", "one"), CacheStatus::ok);
  ASSERT_EQ(cache.set("beta", "two"), CacheStatus::ok);
  ASSERT_EQ(cache.erase("alpha"), CacheStatus::ok);

  std::string output;
  EXPECT_EQ(cache.get("alpha", output), CacheStatus::not_found);
  EXPECT_EQ(cache.get("beta", output), CacheStatus::ok);
  EXPECT_EQ(output, "two");
  EXPECT_EQ(cache.size(), 1U);
}

TEST(CacheTest, FailedInvalidSetDoesNotCorruptExistingState) {
  Cache cache;
  ASSERT_EQ(cache.set("stable", "original"), CacheStatus::ok);
  const std::string oversized_value(Cache::max_value_length + 1, 'v');

  EXPECT_EQ(cache.set("stable", oversized_value), CacheStatus::value_too_large);

  std::string output;
  EXPECT_EQ(cache.get("stable", output), CacheStatus::ok);
  EXPECT_EQ(output, "original");
  EXPECT_EQ(cache.size(), 1U);
}

TEST(CacheTest, OwnsInputKeysAndValues) {
  Cache cache;
  std::string key = "key";
  std::string value = "value";
  ASSERT_EQ(cache.set(key, value), CacheStatus::ok);

  key = "changed-key";
  value = "changed-value";

  std::string output;
  EXPECT_EQ(cache.get("key", output), CacheStatus::ok);
  EXPECT_EQ(output, "value");
}

TEST(CacheTest, MatchesReferenceModelForOneHundredThousandOperations) {
  constexpr std::size_t operation_count = 100'000;
  std::mt19937 generator(0x5EED1234U);
  std::uniform_int_distribution<int> operation_distribution(0, 2);
  std::uniform_int_distribution<int> key_distribution(0, 511);

  Cache cache;
  std::unordered_map<std::string, std::string> reference;

  for (std::size_t operation = 0; operation < operation_count; ++operation) {
    const auto key = "key-" + std::to_string(key_distribution(generator));

    switch (operation_distribution(generator)) {
    case 0: {
      const auto value = generator() % 11 == 0
                             ? std::string{}
                             : "value-" + std::to_string(generator());
      EXPECT_EQ(cache.set(key, value), CacheStatus::ok)
          << "operation " << operation;
      reference[key] = value;
      break;
    }
    case 1: {
      std::string output = "unchanged";
      const auto expected = reference.find(key);
      if (expected == reference.end()) {
        EXPECT_EQ(cache.get(key, output), CacheStatus::not_found)
            << "operation " << operation;
        EXPECT_EQ(output, "unchanged") << "operation " << operation;
      } else {
        EXPECT_EQ(cache.get(key, output), CacheStatus::ok)
            << "operation " << operation;
        EXPECT_EQ(output, expected->second) << "operation " << operation;
      }
      break;
    }
    case 2: {
      const auto expected =
          reference.erase(key) == 0 ? CacheStatus::not_found : CacheStatus::ok;
      EXPECT_EQ(cache.erase(key), expected) << "operation " << operation;
      break;
    }
    default:
      FAIL() << "invalid generated operation";
    }
  }

  EXPECT_EQ(cache.size(), reference.size());
  for (const auto &[key, value] : reference) {
    std::string output;
    EXPECT_EQ(cache.get(key, output), CacheStatus::ok);
    EXPECT_EQ(output, value);
  }
}

} // namespace
} // namespace highcache
