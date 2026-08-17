#include "highcache/net/buffer.h"

#include <gtest/gtest.h>

#include <array>
#include <string>
#include <string_view>

namespace highcache {
namespace {

TEST(BufferTest, StartsEmpty) {
  const Buffer buffer;

  EXPECT_TRUE(buffer.empty());
  EXPECT_EQ(buffer.readable_bytes(), 0U);
  EXPECT_EQ(buffer.data(), nullptr);
  EXPECT_TRUE(buffer.readable_view().empty());
}

TEST(BufferTest, AppendsAndRetrievesPartialData) {
  Buffer buffer;
  buffer.append("abcdef");

  EXPECT_EQ(buffer.readable_view(), "abcdef");
  buffer.retrieve(2);
  EXPECT_EQ(buffer.readable_view(), "cdef");
  buffer.retrieve(4);
  EXPECT_TRUE(buffer.empty());
}

TEST(BufferTest, PreservesBinaryBytes) {
  constexpr std::array<char, 5> bytes{'a', '\0', 'b', '\0', 'c'};
  Buffer buffer;
  buffer.append(bytes.data(), bytes.size());

  EXPECT_EQ(buffer.readable_bytes(), bytes.size());
  EXPECT_EQ(buffer.readable_view(),
            std::string_view(bytes.data(), bytes.size()));
}

TEST(BufferTest, GrowsBeyondInitialCapacity) {
  Buffer buffer(4);
  const std::string value(32 * 1024, 'x');

  buffer.append(value);

  EXPECT_EQ(buffer.readable_bytes(), value.size());
  EXPECT_EQ(buffer.readable_view(), value);
}

TEST(BufferTest, CompactsConsumedSpaceForLaterSegments) {
  Buffer buffer(8);
  buffer.append("12345678");
  buffer.retrieve(6);
  buffer.append("abcdefgh");
  buffer.append("XYZ");

  EXPECT_EQ(buffer.readable_view(), "78abcdefghXYZ");
  buffer.retrieve_all();
  EXPECT_TRUE(buffer.empty());
}

} // namespace
} // namespace highcache
