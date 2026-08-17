#include "highcache/common/error.h"
#include "highcache/config/config.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace highcache {
namespace {

TEST(ConfigTest, UsesInfoLoggingByDefault) {
  const Config config;

  EXPECT_EQ(config.log_level(), LogLevel::info);
  EXPECT_EQ(config.host(), "127.0.0.1");
  EXPECT_EQ(config.port(), 11211U);
  EXPECT_EQ(config.worker_threads(), 4U);
}

TEST(ConfigTest, ParsesWhitespaceAndComments) {
  std::istringstream input("\n  # HighCache settings\n log_level = debug \n"
                           " host = 0.0.0.0\n port = 0\n worker_threads = 8\n");

  const auto config = Config::from_stream(input, "test.conf");

  EXPECT_EQ(config.log_level(), LogLevel::debug);
  EXPECT_EQ(config.host(), "0.0.0.0");
  EXPECT_EQ(config.port(), 0U);
  EXPECT_EQ(config.worker_threads(), 8U);
}

TEST(ConfigTest, LoadsConfigurationFromFile) {
  const auto path =
      std::filesystem::temp_directory_path() / "highcache_config_test.conf";
  {
    std::ofstream output(path);
    ASSERT_TRUE(output.is_open());
    output << "log_level=error\n";
  }

  const auto config = Config::from_file(path);
  std::error_code ignored;
  std::filesystem::remove(path, ignored);

  EXPECT_EQ(config.log_level(), LogLevel::error);
}

TEST(ConfigTest, RejectsMalformedLines) {
  std::istringstream input("log_level info\n");

  try {
    static_cast<void>(Config::from_stream(input, "broken.conf"));
    FAIL() << "expected HighCacheError";
  } catch (const HighCacheError &error) {
    EXPECT_EQ(error.code(), ErrorCode::config_parse);
    EXPECT_STREQ(error.what(), "broken.conf:1: expected key=value");
  }
}

TEST(ConfigTest, RejectsUnknownKeys) {
  std::istringstream input("unknown=8080\n");

  EXPECT_THROW(static_cast<void>(Config::from_stream(input)), HighCacheError);
}

TEST(ConfigTest, RejectsDuplicateKeys) {
  std::istringstream input("log_level=info\nlog_level=debug\n");

  EXPECT_THROW(static_cast<void>(Config::from_stream(input)), HighCacheError);
}

TEST(ConfigTest, RejectsInvalidLogLevel) {
  std::istringstream input("log_level=verbose\n");

  try {
    static_cast<void>(Config::from_stream(input));
    FAIL() << "expected HighCacheError";
  } catch (const HighCacheError &error) {
    EXPECT_EQ(error.code(), ErrorCode::config_parse);
  }
}

TEST(ConfigTest, RejectsInvalidNetworkNumbersAndZeroWorkers) {
  for (const auto *const content :
       {"port=-1\n", "port=65536\n", "port=text\n", "worker_threads=0\n",
        "worker_threads=1025\n"}) {
    std::istringstream input(content);
    EXPECT_THROW(static_cast<void>(Config::from_stream(input)), HighCacheError)
        << content;
  }
}

TEST(ConfigTest, ReportsMissingFiles) {
  const auto path = std::filesystem::temp_directory_path() /
                    "highcache_config_file_that_does_not_exist.conf";
  std::error_code ignored;
  std::filesystem::remove(path, ignored);

  try {
    static_cast<void>(Config::from_file(path));
    FAIL() << "expected HighCacheError";
  } catch (const HighCacheError &error) {
    EXPECT_EQ(error.code(), ErrorCode::config_io);
  }
}

} // namespace
} // namespace highcache
