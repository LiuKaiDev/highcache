#pragma once

#include "highcache/protocol/protocol_codec.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace highcache::benchmark {

struct Options final {
  std::string host{"127.0.0.1"};
  std::uint16_t port{11211};
  std::size_t threads{8};
  std::size_t connections{128};
  std::size_t requests{1'000'000};
  double get_ratio{0.8};
  std::size_t value_size{256};
  std::size_t key_space{100'000};
  std::uint64_t seed{12345};
  std::size_t warmup_requests{10'000};
  std::size_t pipeline{1};
  bool csv{false};
  bool help{false};
};

struct Operation final {
  protocol::Opcode opcode{protocol::Opcode::get};
  std::size_t key_index{0};
  std::uint64_t request_id{0};

  bool operator==(const Operation &) const = default;
};

struct LatencySummary final {
  double average_us{0.0};
  double p50_us{0.0};
  double p95_us{0.0};
  double p99_us{0.0};
};

struct Result final {
  std::size_t requests{0};
  std::size_t successful{0};
  std::size_t failed{0};
  std::size_t get_hits{0};
  std::size_t get_misses{0};
  std::chrono::duration<double> duration{};
  double throughput_rps{0.0};
  LatencySummary latency;
  std::vector<std::string> errors;
};

class Statistics final {
public:
  void record_response(protocol::Opcode opcode, protocol::WireStatus status,
                       std::chrono::nanoseconds latency);
  void record_failures(std::size_t count) noexcept;
  void record_error(std::string message);
  void merge(Statistics other);

  [[nodiscard]] std::size_t successful() const noexcept;
  [[nodiscard]] std::size_t failed() const noexcept;
  [[nodiscard]] std::size_t handled() const noexcept;
  [[nodiscard]] Result finish(std::chrono::duration<double> duration) const;

private:
  std::size_t successful_{0};
  std::size_t failed_{0};
  std::size_t get_hits_{0};
  std::size_t get_misses_{0};
  std::vector<std::chrono::nanoseconds> latencies_;
  std::vector<std::string> errors_;
};

class RequestTracker final {
public:
  using Clock = std::chrono::steady_clock;

  void add(std::uint64_t request_id, protocol::Opcode opcode,
           Clock::time_point start);
  void complete(const protocol::Response &response, Clock::time_point finish,
                Statistics &statistics);
  [[nodiscard]] std::size_t fail_all(Statistics &statistics) noexcept;
  [[nodiscard]] std::size_t size() const noexcept;
  [[nodiscard]] bool empty() const noexcept;

private:
  struct PendingRequest final {
    protocol::Opcode opcode;
    Clock::time_point start;
  };

  std::unordered_map<std::uint64_t, PendingRequest> pending_;
};

[[nodiscard]] Options parse_options(std::span<const std::string_view> args);
void validate_options(const Options &options);
[[nodiscard]] std::vector<Operation>
generate_workload(std::size_t request_count, double get_ratio,
                  std::size_t key_space, std::uint64_t seed,
                  std::uint64_t first_request_id = 1);
[[nodiscard]] LatencySummary
summarize_latencies(std::vector<std::chrono::nanoseconds> latencies);
[[nodiscard]] Result run_benchmark(const Options &options);
void print_result(std::ostream &output, const Options &options,
                  const Result &result);
void print_usage(std::ostream &output);

} // namespace highcache::benchmark
