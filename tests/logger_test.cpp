#include "highcache/common/error.h"
#include "highcache/logging/logger.h"

#include <gtest/gtest.h>

#include <sstream>

namespace highcache {
namespace {

TEST(LoggerTest, WritesLevelAndMessage) {
  std::ostringstream output;
  Logger logger(output);

  logger.info("server ready");

  EXPECT_EQ(output.str(), "[INFO] server ready\n");
}

TEST(LoggerTest, FiltersMessagesBelowMinimumLevel) {
  std::ostringstream output;
  Logger logger(output, LogLevel::warning);

  logger.debug("debug details");
  logger.info("startup details");
  logger.warning("configuration warning");
  logger.error("startup failed");

  EXPECT_EQ(output.str(),
            "[WARNING] configuration warning\n[ERROR] startup failed\n");
}

TEST(LoggerTest, UpdatesMinimumLevel) {
  std::ostringstream output;
  Logger logger(output, LogLevel::error);

  logger.set_minimum_level(LogLevel::debug);
  logger.debug("enabled");

  EXPECT_EQ(logger.minimum_level(), LogLevel::debug);
  EXPECT_EQ(output.str(), "[DEBUG] enabled\n");
}

TEST(LoggerTest, ParsesSupportedLevels) {
  EXPECT_EQ(parse_log_level("debug"), LogLevel::debug);
  EXPECT_EQ(parse_log_level("info"), LogLevel::info);
  EXPECT_EQ(parse_log_level("warning"), LogLevel::warning);
  EXPECT_EQ(parse_log_level("error"), LogLevel::error);
}

TEST(LoggerTest, RejectsUnknownLevels) {
  try {
    static_cast<void>(parse_log_level("trace"));
    FAIL() << "expected HighCacheError";
  } catch (const HighCacheError &error) {
    EXPECT_EQ(error.code(), ErrorCode::invalid_argument);
  }
}

} // namespace
} // namespace highcache
