#include "highcache/common/error.h"

#include <gtest/gtest.h>

namespace highcache {
namespace {

TEST(ErrorTest, PreservesCodeAndMessage) {
  const HighCacheError error(ErrorCode::config_parse, "invalid setting");

  EXPECT_EQ(error.code(), ErrorCode::config_parse);
  EXPECT_STREQ(error.what(), "invalid setting");
}

TEST(ErrorTest, ProvidesStableCodeNames) {
  EXPECT_EQ(to_string(ErrorCode::invalid_argument), "invalid_argument");
  EXPECT_EQ(to_string(ErrorCode::config_io), "config_io");
  EXPECT_EQ(to_string(ErrorCode::config_parse), "config_parse");
}

} // namespace
} // namespace highcache
