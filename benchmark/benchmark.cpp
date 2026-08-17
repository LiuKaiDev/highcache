#include "benchmark/benchmark.h"

#include "highcache/cache/cache.h"
#include "highcache/net/buffer.h"
#include "highcache/net/unique_fd.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/socket.h>

#include <algorithm>
#include <array>
#include <barrier>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iterator>
#include <limits>
#include <numeric>
#include <ostream>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace highcache::benchmark {
namespace {

constexpr std::size_t max_events = 64;
constexpr std::size_t read_chunk_size = 64 * 1024;
constexpr int io_timeout_ms = 30'000;

[[noreturn]] void throw_system_error(const std::string_view operation) {
  throw std::runtime_error(std::string(operation) + ": " +
                           std::system_category().message(errno));
}

std::uint64_t parse_unsigned(const std::string_view value,
                             const std::string_view option) {
  std::uint64_t parsed = 0;
  const auto [end, error] =
      std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (error != std::errc{} || end != value.data() + value.size()) {
    throw std::invalid_argument(std::string(option) +
                                " expects an unsigned integer");
  }
  return parsed;
}

double parse_ratio(const std::string_view value,
                   const std::string_view option) {
  double parsed = 0.0;
  const auto [end, error] =
      std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (error != std::errc{} || end != value.data() + value.size() ||
      !std::isfinite(parsed)) {
    throw std::invalid_argument(std::string(option) +
                                " expects a finite decimal number");
  }
  return parsed;
}

std::string_view require_value(const std::span<const std::string_view> args,
                               std::size_t &index) {
  if (index + 1 >= args.size()) {
    throw std::invalid_argument(std::string(args[index]) +
                                " requires a value");
  }
  return args[++index];
}

std::size_t checked_size(const std::uint64_t value,
                         const std::string_view option) {
  if (value > std::numeric_limits<std::size_t>::max()) {
    throw std::invalid_argument(std::string(option) + " is too large");
  }
  return static_cast<std::size_t>(value);
}

UniqueFd connect_socket(const Options &options) {
  const int raw_socket = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (raw_socket < 0) {
    throw_system_error("socket failed");
  }
  UniqueFd socket(raw_socket);

  constexpr int enabled = 1;
  if (::setsockopt(socket.get(), IPPROTO_TCP, TCP_NODELAY, &enabled,
                   sizeof(enabled)) != 0) {
    throw_system_error("setsockopt TCP_NODELAY failed");
  }

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(options.port);
  if (::inet_pton(AF_INET, options.host.c_str(), &address.sin_addr) != 1) {
    throw std::invalid_argument("benchmark host must be an IPv4 address");
  }
  if (::connect(socket.get(), reinterpret_cast<const sockaddr *>(&address),
                sizeof(address)) != 0) {
    throw_system_error("connect failed");
  }

  const int flags = ::fcntl(socket.get(), F_GETFL, 0);
  if (flags < 0 || ::fcntl(socket.get(), F_SETFL, flags | O_NONBLOCK) != 0) {
    throw_system_error("fcntl O_NONBLOCK failed");
  }
  return socket;
}

UniqueFd create_epoll() {
  const int fd = ::epoll_create1(EPOLL_CLOEXEC);
  if (fd < 0) {
    throw_system_error("epoll_create1 failed");
  }
  return UniqueFd(fd);
}

void add_to_epoll(const int epoll_fd, const int fd,
                  const std::uint32_t events) {
  epoll_event event{};
  event.events = events;
  event.data.fd = fd;
  if (::epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &event) != 0) {
    throw_system_error("epoll_ctl add failed");
  }
}

void modify_epoll(const int epoll_fd, const int fd,
                  const std::uint32_t events) {
  epoll_event event{};
  event.events = events;
  event.data.fd = fd;
  if (::epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &event) != 0) {
    throw_system_error("epoll_ctl modify failed");
  }
}

void remove_from_epoll(const int epoll_fd, const int fd) noexcept {
  static_cast<void>(::epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr));
}

std::vector<std::string> make_keys(const std::size_t key_space) {
  std::vector<std::string> keys;
  keys.reserve(key_space);
  for (std::size_t index = 0; index < key_space; ++index) {
    keys.push_back("highcache-bench-" + std::to_string(index));
  }
  return keys;
}

std::string make_value(const std::size_t size, const std::uint64_t seed) {
  std::string value(size, '\0');
  std::mt19937_64 generator(seed ^ 0x56414C5545554C4CULL);
  for (auto &byte : value) {
    byte = static_cast<char>(generator() & 0xFFU);
  }
  return value;
}

std::vector<Operation> make_preload(const std::size_t key_space) {
  std::vector<Operation> preload;
  preload.reserve(key_space);
  for (std::size_t index = 0; index < key_space; ++index) {
    preload.push_back(
        {protocol::Opcode::set, index, static_cast<std::uint64_t>(index + 1)});
  }
  return preload;
}

struct ClientConnection final {
  explicit ClientConnection(UniqueFd descriptor)
      : socket(std::move(descriptor)) {}

  UniqueFd socket;
  Buffer input;
  Buffer output;
  RequestTracker tracker;
  bool active{true};
};

class ClientWorker final {
public:
  ClientWorker(const Options &options, const std::vector<Operation> &plan,
               const std::vector<std::string> &keys,
               const std::string &value, const std::size_t begin,
               const std::size_t end, const std::size_t connection_count)
      : options_(options), plan_(plan), keys_(keys), value_(value), begin_(begin),
        end_(end), next_(begin), epoll_(create_epoll()) {
    connections_.reserve(connection_count);
    for (std::size_t index = 0; index < connection_count; ++index) {
      connections_.emplace_back(connect_socket(options_));
      const int fd = connections_.back().socket.get();
      connection_indexes_.emplace(fd, connections_.size() - 1);
      add_to_epoll(epoll_.get(), fd, EPOLLIN | EPOLLRDHUP);
    }
  }

  [[nodiscard]] Statistics run() noexcept {
    try {
      for (auto &connection : connections_) {
        fill_pipeline(connection);
        update_interest(connection);
      }

      std::array<epoll_event, max_events> events{};
      while (statistics_.handled() < end_ - begin_) {
        if (active_connection_count() == 0) {
          fail_unissued();
          break;
        }

        const int count = ::epoll_wait(epoll_.get(), events.data(),
                                       static_cast<int>(events.size()),
                                       io_timeout_ms);
        if (count < 0) {
          if (errno == EINTR) {
            continue;
          }
          throw_system_error("benchmark epoll_wait failed");
        }
        if (count == 0) {
          throw std::runtime_error("benchmark I/O timed out");
        }

        for (int event_index = 0; event_index < count; ++event_index) {
          const auto event = events[static_cast<std::size_t>(event_index)];
          const auto found = connection_indexes_.find(event.data.fd);
          if (found == connection_indexes_.end()) {
            continue;
          }
          auto &connection = connections_[found->second];
          if (!connection.active) {
            continue;
          }

          try {
            bool keep = (event.events & EPOLLERR) == 0;
            if (keep && (event.events & EPOLLOUT) != 0) {
              keep = handle_writable(connection);
            }
            if (keep &&
                (event.events & (EPOLLIN | EPOLLRDHUP | EPOLLHUP)) != 0) {
              keep = handle_readable(connection);
            }
            if (keep && (event.events & (EPOLLRDHUP | EPOLLHUP)) != 0) {
              keep = false;
            }
            if (!keep) {
              fail_connection(connection, "benchmark connection closed");
              continue;
            }

            fill_pipeline(connection);
            update_interest(connection);
          } catch (const std::exception &error) {
            fail_connection(connection, error.what());
          }
        }
      }
    } catch (const std::exception &error) {
      statistics_.record_error(error.what());
      fail_everything();
    }
    return std::move(statistics_);
  }

private:
  [[nodiscard]] std::size_t active_connection_count() const noexcept {
    return static_cast<std::size_t>(std::count_if(
        connections_.begin(), connections_.end(),
        [](const ClientConnection &connection) { return connection.active; }));
  }

  [[nodiscard]] std::uint32_t interest(
      const ClientConnection &connection) const noexcept {
    std::uint32_t events = EPOLLIN | EPOLLRDHUP;
    if (!connection.output.empty()) {
      events |= EPOLLOUT;
    }
    return events;
  }

  void update_interest(const ClientConnection &connection) {
    modify_epoll(epoll_.get(), connection.socket.get(), interest(connection));
  }

  void fill_pipeline(ClientConnection &connection) {
    while (connection.tracker.size() < options_.pipeline && next_ < end_) {
      const auto &operation = plan_[next_++];
      protocol::Request request;
      request.opcode = operation.opcode;
      request.request_id = operation.request_id;
      request.key = keys_[operation.key_index];
      if (operation.opcode == protocol::Opcode::set) {
        request.value = value_;
      }
      const auto frame = protocol::encode_request(request);
      const auto start = RequestTracker::Clock::now();
      connection.output.append(frame);
      connection.tracker.add(operation.request_id, operation.opcode, start);
    }
  }

  [[nodiscard]] bool handle_writable(ClientConnection &connection) {
    while (!connection.output.empty()) {
      const auto result =
          ::send(connection.socket.get(), connection.output.data(),
                 connection.output.readable_bytes(), MSG_NOSIGNAL);
      if (result > 0) {
        connection.output.retrieve(static_cast<std::size_t>(result));
        continue;
      }
      if (result < 0 && errno == EINTR) {
        continue;
      }
      if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        return true;
      }
      return false;
    }
    return true;
  }

  [[nodiscard]] bool handle_readable(ClientConnection &connection) {
    std::array<char, read_chunk_size> bytes{};
    while (true) {
      const auto result =
          ::recv(connection.socket.get(), bytes.data(), bytes.size(), 0);
      if (result > 0) {
        connection.input.append(bytes.data(), static_cast<std::size_t>(result));
        decode_responses(connection);
        continue;
      }
      if (result == 0) {
        return false;
      }
      if (errno == EINTR) {
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return true;
      }
      return false;
    }
  }

  void decode_responses(ClientConnection &connection) {
    while (!connection.input.empty()) {
      protocol::Response response;
      protocol::ProtocolError error;
      const auto decoded =
          protocol::decode_response(connection.input, response, error);
      if (decoded == protocol::DecodeResult::incomplete) {
        return;
      }
      if (decoded == protocol::DecodeResult::error) {
        throw std::runtime_error("invalid benchmark response: " +
                                 error.message);
      }
      connection.tracker.complete(response, RequestTracker::Clock::now(),
                                  statistics_);
    }
  }

  void fail_connection(ClientConnection &connection,
                       const std::string_view error) noexcept {
    if (!connection.active) {
      return;
    }
    const auto failed = connection.tracker.fail_all(statistics_);
    if (failed != 0) {
      try {
        statistics_.record_error(std::string(error));
      } catch (...) {
      }
    }
    remove_from_epoll(epoll_.get(), connection.socket.get());
    connection.socket.reset();
    connection.active = false;
  }

  void fail_unissued() noexcept {
    if (next_ < end_) {
      statistics_.record_failures(end_ - next_);
      next_ = end_;
    }
  }

  void fail_everything() noexcept {
    for (auto &connection : connections_) {
      if (connection.active) {
        static_cast<void>(connection.tracker.fail_all(statistics_));
        connection.active = false;
        connection.socket.reset();
      }
    }
    fail_unissued();
  }

  const Options &options_;
  const std::vector<Operation> &plan_;
  const std::vector<std::string> &keys_;
  const std::string &value_;
  std::size_t begin_;
  std::size_t end_;
  std::size_t next_;
  UniqueFd epoll_;
  std::vector<ClientConnection> connections_;
  std::unordered_map<int, std::size_t> connection_indexes_;
  Statistics statistics_;
};

struct ExecutionResult final {
  Statistics statistics;
  std::chrono::duration<double> duration;
};

ExecutionResult execute_plan(const Options &options,
                             const std::vector<Operation> &plan,
                             const std::vector<std::string> &keys,
                             const std::string &value) {
  std::vector<Statistics> per_thread(options.threads);
  std::barrier ready(static_cast<std::ptrdiff_t>(options.threads + 1));
  std::barrier start(static_cast<std::ptrdiff_t>(options.threads + 1));
  std::vector<std::thread> threads;
  threads.reserve(options.threads);

  for (std::size_t thread_index = 0; thread_index < options.threads;
       ++thread_index) {
    const auto begin = plan.size() * thread_index / options.threads;
    const auto end = plan.size() * (thread_index + 1) / options.threads;
    const auto base_connections = options.connections / options.threads;
    const auto extra_connections =
        static_cast<std::size_t>(thread_index <
                                 options.connections % options.threads);
    const auto connection_count = base_connections + extra_connections;

    threads.emplace_back([&, thread_index, begin, end, connection_count] {
      try {
        ClientWorker worker(options, plan, keys, value, begin, end,
                            connection_count);
        ready.arrive_and_wait();
        start.arrive_and_wait();
        per_thread[thread_index] = worker.run();
      } catch (const std::exception &error) {
        per_thread[thread_index].record_failures(end - begin);
        per_thread[thread_index].record_error(error.what());
        ready.arrive_and_wait();
        start.arrive_and_wait();
      }
    });
  }

  ready.arrive_and_wait();
  const auto started = std::chrono::steady_clock::now();
  start.arrive_and_wait();
  for (auto &thread : threads) {
    thread.join();
  }
  const auto finished = std::chrono::steady_clock::now();

  Statistics combined;
  for (auto &statistics : per_thread) {
    combined.merge(std::move(statistics));
  }
  return {std::move(combined), finished - started};
}

void require_success(const ExecutionResult &execution,
                     const std::string_view phase) {
  if (execution.statistics.failed() != 0) {
    throw std::runtime_error(std::string(phase) + " failed for " +
                             std::to_string(execution.statistics.failed()) +
                             " requests");
  }
}

double hit_ratio(const Result &result) {
  const auto gets = result.get_hits + result.get_misses;
  return gets == 0
             ? 0.0
             : static_cast<double>(result.get_hits) /
                   static_cast<double>(gets);
}

} // namespace

void Statistics::record_response(const protocol::Opcode opcode,
                                 const protocol::WireStatus status,
                                 const std::chrono::nanoseconds latency) {
  latencies_.push_back(latency);
  if (opcode == protocol::Opcode::get && status == protocol::WireStatus::ok) {
    ++get_hits_;
    ++successful_;
    return;
  }
  if (opcode == protocol::Opcode::get &&
      status == protocol::WireStatus::not_found) {
    ++get_misses_;
    ++successful_;
    return;
  }
  if (opcode == protocol::Opcode::set && status == protocol::WireStatus::ok) {
    ++successful_;
    return;
  }
  ++failed_;
}

void Statistics::record_failures(const std::size_t count) noexcept {
  failed_ += count;
}

void Statistics::record_error(std::string message) {
  errors_.push_back(std::move(message));
}

void Statistics::merge(Statistics other) {
  successful_ += other.successful_;
  failed_ += other.failed_;
  get_hits_ += other.get_hits_;
  get_misses_ += other.get_misses_;
  latencies_.insert(latencies_.end(),
                    std::make_move_iterator(other.latencies_.begin()),
                    std::make_move_iterator(other.latencies_.end()));
  errors_.insert(errors_.end(), std::make_move_iterator(other.errors_.begin()),
                 std::make_move_iterator(other.errors_.end()));
}

std::size_t Statistics::successful() const noexcept { return successful_; }

std::size_t Statistics::failed() const noexcept { return failed_; }

std::size_t Statistics::handled() const noexcept {
  return successful_ + failed_;
}

Result Statistics::finish(const std::chrono::duration<double> duration) const {
  Result result;
  result.requests = handled();
  result.successful = successful_;
  result.failed = failed_;
  result.get_hits = get_hits_;
  result.get_misses = get_misses_;
  result.duration = duration;
  result.throughput_rps = duration.count() > 0.0
                              ? static_cast<double>(result.requests) /
                                    duration.count()
                              : 0.0;
  result.latency = summarize_latencies(latencies_);
  result.errors = errors_;
  return result;
}

void RequestTracker::add(const std::uint64_t request_id,
                         const protocol::Opcode opcode,
                         const Clock::time_point start) {
  if (!pending_.emplace(request_id, PendingRequest{opcode, start}).second) {
    throw std::runtime_error("duplicate benchmark request_id");
  }
}

void RequestTracker::complete(const protocol::Response &response,
                              const Clock::time_point finish,
                              Statistics &statistics) {
  const auto request = pending_.find(response.request_id);
  if (request == pending_.end()) {
    throw std::runtime_error("response has unknown benchmark request_id");
  }
  statistics.record_response(
      request->second.opcode, response.status,
      std::chrono::duration_cast<std::chrono::nanoseconds>(finish -
                                                           request->second.start));
  pending_.erase(request);
}

std::size_t RequestTracker::fail_all(Statistics &statistics) noexcept {
  const auto count = pending_.size();
  statistics.record_failures(count);
  pending_.clear();
  return count;
}

std::size_t RequestTracker::size() const noexcept { return pending_.size(); }

bool RequestTracker::empty() const noexcept { return pending_.empty(); }

Options parse_options(const std::span<const std::string_view> args) {
  Options options;
  for (std::size_t index = 0; index < args.size(); ++index) {
    const auto argument = args[index];
    if (argument == "--help") {
      options.help = true;
    } else if (argument == "--csv") {
      options.csv = true;
    } else if (argument == "--host") {
      options.host = require_value(args, index);
    } else if (argument == "--port") {
      const auto port = parse_unsigned(require_value(args, index), argument);
      if (port == 0 || port > std::numeric_limits<std::uint16_t>::max()) {
        throw std::invalid_argument("--port must be between 1 and 65535");
      }
      options.port = static_cast<std::uint16_t>(port);
    } else if (argument == "--threads") {
      options.threads = checked_size(
          parse_unsigned(require_value(args, index), argument), argument);
    } else if (argument == "--connections") {
      options.connections = checked_size(
          parse_unsigned(require_value(args, index), argument), argument);
    } else if (argument == "--requests") {
      options.requests = checked_size(
          parse_unsigned(require_value(args, index), argument), argument);
    } else if (argument == "--get-ratio") {
      options.get_ratio = parse_ratio(require_value(args, index), argument);
    } else if (argument == "--value-size") {
      options.value_size = checked_size(
          parse_unsigned(require_value(args, index), argument), argument);
    } else if (argument == "--key-space") {
      options.key_space = checked_size(
          parse_unsigned(require_value(args, index), argument), argument);
    } else if (argument == "--seed") {
      options.seed = parse_unsigned(require_value(args, index), argument);
    } else if (argument == "--warmup-requests") {
      options.warmup_requests = checked_size(
          parse_unsigned(require_value(args, index), argument), argument);
    } else if (argument == "--pipeline") {
      options.pipeline = checked_size(
          parse_unsigned(require_value(args, index), argument), argument);
    } else {
      throw std::invalid_argument("unknown benchmark option: " +
                                  std::string(argument));
    }
  }
  if (!options.help) {
    validate_options(options);
  }
  return options;
}

void validate_options(const Options &options) {
  if (options.host.empty()) {
    throw std::invalid_argument("--host must not be empty");
  }
  if (options.port == 0) {
    throw std::invalid_argument("--port must be between 1 and 65535");
  }
  if (options.threads == 0) {
    throw std::invalid_argument("--threads must be greater than zero");
  }
  if (options.connections == 0 || options.connections < options.threads) {
    throw std::invalid_argument(
        "--connections must be at least the thread count");
  }
  if (options.requests == 0) {
    throw std::invalid_argument("--requests must be greater than zero");
  }
  if (options.get_ratio < 0.0 || options.get_ratio > 1.0) {
    throw std::invalid_argument("--get-ratio must be between 0 and 1");
  }
  if (options.value_size > Cache::max_value_length) {
    throw std::invalid_argument("--value-size exceeds the cache maximum");
  }
  if (options.key_space == 0) {
    throw std::invalid_argument("--key-space must be greater than zero");
  }
  if (options.pipeline == 0 || options.pipeline > 1024) {
    throw std::invalid_argument("--pipeline must be between 1 and 1024");
  }
  const auto maximum_id_count = std::numeric_limits<std::uint64_t>::max() - 1;
  if (options.key_space > maximum_id_count ||
      options.warmup_requests > maximum_id_count - options.key_space ||
      options.requests >
          maximum_id_count - options.key_space - options.warmup_requests) {
    throw std::invalid_argument("benchmark request_id range overflows");
  }
}

std::vector<Operation> generate_workload(const std::size_t request_count,
                                         const double get_ratio,
                                         const std::size_t key_space,
                                         const std::uint64_t seed,
                                         const std::uint64_t first_request_id) {
  if (key_space == 0 || get_ratio < 0.0 || get_ratio > 1.0 ||
      !std::isfinite(get_ratio)) {
    throw std::invalid_argument("invalid workload generation parameters");
  }
  if (request_count != 0 &&
      first_request_id > std::numeric_limits<std::uint64_t>::max() -
                             (request_count - 1)) {
    throw std::invalid_argument("workload request_id range overflows");
  }

  const auto get_count = static_cast<std::size_t>(std::llround(
      static_cast<double>(request_count) * get_ratio));
  std::vector<protocol::Opcode> opcodes(request_count, protocol::Opcode::set);
  std::fill_n(opcodes.begin(), get_count, protocol::Opcode::get);

  std::mt19937_64 generator(seed);
  std::shuffle(opcodes.begin(), opcodes.end(), generator);
  std::uniform_int_distribution<std::size_t> key_distribution(0,
                                                               key_space - 1);

  std::vector<Operation> operations;
  operations.reserve(request_count);
  for (std::size_t index = 0; index < request_count; ++index) {
    operations.push_back(
        {opcodes[index], key_distribution(generator), first_request_id + index});
  }
  return operations;
}

LatencySummary summarize_latencies(
    std::vector<std::chrono::nanoseconds> latencies) {
  if (latencies.empty()) {
    return {};
  }
  std::sort(latencies.begin(), latencies.end());
  const auto percentile = [&](const double quantile) {
    const auto rank = static_cast<std::size_t>(std::ceil(
        quantile * static_cast<double>(latencies.size())));
    const auto index = std::max<std::size_t>(1, rank) - 1;
    return std::chrono::duration<double, std::micro>(latencies[index]).count();
  };
  const auto total = std::accumulate(
      latencies.begin(), latencies.end(), std::chrono::nanoseconds::zero());
  return {std::chrono::duration<double, std::micro>(total).count() /
              static_cast<double>(latencies.size()),
          percentile(0.50), percentile(0.95), percentile(0.99)};
}

Result run_benchmark(const Options &options) {
  validate_options(options);
  const auto keys = make_keys(options.key_space);
  const auto value = make_value(options.value_size, options.seed);

  const auto preload = make_preload(options.key_space);
  require_success(execute_plan(options, preload, keys, value), "preload");

  const auto warmup_first_id =
      static_cast<std::uint64_t>(options.key_space) + 1;
  if (options.warmup_requests != 0) {
    const auto warmup = generate_workload(
        options.warmup_requests, options.get_ratio, options.key_space,
        options.seed ^ 0x5741524D55504C4CULL, warmup_first_id);
    require_success(execute_plan(options, warmup, keys, value), "warmup");
  }

  const auto measured_first_id =
      warmup_first_id + static_cast<std::uint64_t>(options.warmup_requests);
  const auto measured = generate_workload(
      options.requests, options.get_ratio, options.key_space, options.seed,
      measured_first_id);
  auto execution = execute_plan(options, measured, keys, value);
  return execution.statistics.finish(execution.duration);
}

void print_result(std::ostream &output, const Options &options,
                  const Result &result) {
  output << std::fixed << std::setprecision(3);
  if (options.csv) {
    output << "requests,successful,failed,threads,connections,value_size,"
              "get_ratio,duration_seconds,throughput_rps,avg_us,p50_us,p95_us,"
              "p99_us,get_hits,get_misses,hit_ratio,key_space,seed,pipeline,"
              "warmup_requests,error_count\n";
    output << result.requests << ',' << result.successful << ',' << result.failed
           << ',' << options.threads << ',' << options.connections << ','
           << options.value_size << ',' << options.get_ratio << ','
           << result.duration.count() << ',' << result.throughput_rps << ','
           << result.latency.average_us << ',' << result.latency.p50_us << ','
           << result.latency.p95_us << ',' << result.latency.p99_us << ','
           << result.get_hits << ',' << result.get_misses << ','
           << hit_ratio(result) << ',' << options.key_space << ','
           << options.seed << ',' << options.pipeline << ','
           << options.warmup_requests << ',' << result.errors.size() << '\n';
    return;
  }

  output << "Requests: " << result.requests << '\n'
         << "Successful: " << result.successful << '\n'
         << "Failed: " << result.failed << '\n'
         << "Threads: " << options.threads << '\n'
         << "Connections: " << options.connections << '\n'
         << "Value Size: " << options.value_size << '\n'
         << "GET Ratio: " << options.get_ratio << '\n'
         << "Duration: " << result.duration.count() << " s\n"
         << "Throughput: " << result.throughput_rps << " req/s\n\n"
         << "Latency:\n"
         << "  AVG: " << result.latency.average_us << " us\n"
         << "  P50: " << result.latency.p50_us << " us\n"
         << "  P95: " << result.latency.p95_us << " us\n"
         << "  P99: " << result.latency.p99_us << " us\n\n"
         << "GET:\n"
         << "  Hits: " << result.get_hits << '\n'
         << "  Misses: " << result.get_misses << '\n'
         << "  Hit Ratio: " << hit_ratio(result) << '\n';
  for (const auto &error : result.errors) {
    output << "Error: " << error << '\n';
  }
}

void print_usage(std::ostream &output) {
  output << "usage: highcache_benchmark [options]\n"
            "  --host HOST\n"
            "  --port PORT\n"
            "  --threads COUNT\n"
            "  --connections COUNT\n"
            "  --requests COUNT\n"
            "  --get-ratio RATIO\n"
            "  --value-size BYTES\n"
            "  --key-space COUNT\n"
            "  --seed VALUE\n"
            "  --warmup-requests COUNT\n"
            "  --pipeline DEPTH\n"
            "  --csv\n";
}

} // namespace highcache::benchmark
