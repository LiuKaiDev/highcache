#include "benchmark/benchmark.h"

#include "highcache/cache/cache_engine.h"
#include "highcache/net/cache_server.h"
#include "highcache/net/connection.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace highcache::benchmark {
namespace {

Options parse(const std::vector<std::string_view> &arguments) {
  return parse_options(std::span<const std::string_view>(arguments));
}

TEST(BenchmarkOptionsTest, ParsesSupportedArguments) {
  const std::vector<std::string_view> arguments{
      "--host",            "127.0.0.2", "--port",      "12000",
      "--threads",         "3",         "--connections", "9",
      "--requests",        "1234",      "--get-ratio", "0.5",
      "--value-size",      "1024",      "--key-space", "77",
      "--seed",            "42",        "--warmup-requests", "18",
      "--pipeline",        "7",         "--csv"};

  const auto options = parse(arguments);

  EXPECT_EQ(options.host, "127.0.0.2");
  EXPECT_EQ(options.port, 12000U);
  EXPECT_EQ(options.threads, 3U);
  EXPECT_EQ(options.connections, 9U);
  EXPECT_EQ(options.requests, 1234U);
  EXPECT_DOUBLE_EQ(options.get_ratio, 0.5);
  EXPECT_EQ(options.value_size, 1024U);
  EXPECT_EQ(options.key_space, 77U);
  EXPECT_EQ(options.seed, 42U);
  EXPECT_EQ(options.warmup_requests, 18U);
  EXPECT_EQ(options.pipeline, 7U);
  EXPECT_TRUE(options.csv);
}

TEST(BenchmarkOptionsTest, RejectsInvalidAndOverflowingArguments) {
  EXPECT_THROW(parse({"--port", "65537"}), std::invalid_argument);
  EXPECT_THROW(parse({"--threads", "0"}), std::invalid_argument);
  EXPECT_THROW(parse({"--threads", "3", "--connections", "2"}),
               std::invalid_argument);
  EXPECT_THROW(parse({"--get-ratio", "1.01"}), std::invalid_argument);
  EXPECT_THROW(parse({"--pipeline", "0"}), std::invalid_argument);
  EXPECT_THROW(parse({"--unknown"}), std::invalid_argument);
  EXPECT_THROW(parse({"--requests"}), std::invalid_argument);
  EXPECT_NO_THROW(parse({"--help", "--threads", "0"}));
}

TEST(BenchmarkLatencyTest, UsesNearestRankPercentilesAndObservedAverage) {
  using namespace std::chrono_literals;
  const auto summary = summarize_latencies({1us, 2us, 3us, 4us, 5us});

  EXPECT_DOUBLE_EQ(summary.average_us, 3.0);
  EXPECT_DOUBLE_EQ(summary.p50_us, 3.0);
  EXPECT_DOUBLE_EQ(summary.p95_us, 5.0);
  EXPECT_DOUBLE_EQ(summary.p99_us, 5.0);
}

TEST(BenchmarkWorkloadTest, ProducesExactMixAndFixedSeedSequence) {
  const auto first = generate_workload(100, 0.8, 17, 12345, 900);
  const auto repeated = generate_workload(100, 0.8, 17, 12345, 900);
  const auto other_seed = generate_workload(100, 0.8, 17, 54321, 900);

  EXPECT_EQ(first, repeated);
  EXPECT_NE(first, other_seed);
  EXPECT_EQ(std::count_if(first.begin(), first.end(), [](const Operation &op) {
              return op.opcode == protocol::Opcode::get;
            }),
            80);
  for (std::size_t index = 0; index < first.size(); ++index) {
    EXPECT_LT(first[index].key_index, 17U);
    EXPECT_EQ(first[index].request_id, 900U + index);
  }
}

TEST(BenchmarkWorkloadTest, RejectsRequestIdOverflow) {
  EXPECT_THROW(generate_workload(2, 1.0, 1, 1,
                                 std::numeric_limits<std::uint64_t>::max()),
               std::invalid_argument);
}

TEST(BenchmarkStatisticsTest, CountsResponsesFailuresAndErrors) {
  using namespace std::chrono_literals;
  Statistics statistics;
  statistics.record_response(protocol::Opcode::get, protocol::WireStatus::ok,
                             1us);
  statistics.record_response(protocol::Opcode::get,
                             protocol::WireStatus::not_found, 2us);
  statistics.record_response(protocol::Opcode::set, protocol::WireStatus::ok,
                             3us);
  statistics.record_response(protocol::Opcode::set,
                             protocol::WireStatus::internal_error, 4us);
  statistics.record_failures(2);
  statistics.record_error("socket failed");

  const auto result = statistics.finish(1s);
  EXPECT_EQ(result.requests, 6U);
  EXPECT_EQ(result.successful, 3U);
  EXPECT_EQ(result.failed, 3U);
  EXPECT_EQ(result.get_hits, 1U);
  EXPECT_EQ(result.get_misses, 1U);
  EXPECT_DOUBLE_EQ(result.throughput_rps, 6.0);
  ASSERT_EQ(result.errors.size(), 1U);
  EXPECT_EQ(result.errors.front(), "socket failed");
}

TEST(BenchmarkRequestTrackerTest, CorrelatesOutOfOrderResponsesById) {
  using namespace std::chrono_literals;
  RequestTracker tracker;
  Statistics statistics;
  const auto start = RequestTracker::Clock::now();
  tracker.add(11, protocol::Opcode::get, start);
  tracker.add(12, protocol::Opcode::set, start + 1us);

  tracker.complete({12, protocol::WireStatus::ok, {}}, start + 4us,
                   statistics);
  tracker.complete({11, protocol::WireStatus::ok, "value"}, start + 5us,
                   statistics);

  EXPECT_TRUE(tracker.empty());
  const auto result = statistics.finish(1s);
  EXPECT_EQ(result.successful, 2U);
  EXPECT_EQ(result.get_hits, 1U);
  EXPECT_THROW(tracker.complete({99, protocol::WireStatus::ok, {}}, start,
                                statistics),
               std::runtime_error);
}

TEST(BenchmarkIntegrationTest, ExecutesAcrossMultipleTcpConnections) {
  CacheEngine engine(64 * 1024 * 1024, 64);
  CacheServer server(engine,
                     {"127.0.0.1", 0, 2, Connection::default_output_limit});
  server.start();

  Options options;
  options.host = server.host();
  options.port = server.port();
  options.threads = 2;
  options.connections = 4;
  options.requests = 200;
  options.get_ratio = 0.5;
  options.value_size = 64;
  options.key_space = 16;
  options.seed = 8128;
  options.warmup_requests = 20;
  options.pipeline = 4;

  const auto result = run_benchmark(options);
  server.request_stop();
  server.join();

  EXPECT_EQ(result.requests, 200U);
  EXPECT_EQ(result.successful, 200U);
  EXPECT_EQ(result.failed, 0U);
  EXPECT_EQ(result.get_hits, 100U);
  EXPECT_EQ(result.get_misses, 0U);
  EXPECT_TRUE(result.errors.empty());
}

} // namespace
} // namespace highcache::benchmark
