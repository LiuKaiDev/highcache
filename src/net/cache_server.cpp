#include "highcache/net/cache_server.h"

#include "highcache/common/error.h"
#include "highcache/net/connection.h"
#include "highcache/net/unique_fd.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace highcache {
namespace {

constexpr std::size_t max_events = 64;

[[noreturn]] void throw_network_error(const std::string &operation,
                                      const int error_number = errno) {
  throw HighCacheError(ErrorCode::network,
                       operation + ": " +
                           std::system_category().message(error_number));
}

UniqueFd create_epoll() {
  const int fd = ::epoll_create1(EPOLL_CLOEXEC);
  if (fd < 0) {
    throw_network_error("epoll_create1 failed");
  }
  return UniqueFd(fd);
}

UniqueFd create_event() {
  const int fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (fd < 0) {
    throw_network_error("eventfd failed");
  }
  return UniqueFd(fd);
}

UniqueFd create_timer() {
  const int fd = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
  if (fd < 0) {
    throw_network_error("timerfd_create failed");
  }
  return UniqueFd(fd);
}

void add_to_epoll(const int epoll_fd, const int fd,
                  const std::uint32_t events) {
  epoll_event event{};
  event.events = events;
  event.data.fd = fd;
  if (::epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &event) != 0) {
    throw_network_error("epoll_ctl add failed");
  }
}

bool modify_epoll(const int epoll_fd, const int fd,
                  const std::uint32_t events) noexcept {
  epoll_event event{};
  event.events = events;
  event.data.fd = fd;
  return ::epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &event) == 0;
}

void remove_from_epoll(const int epoll_fd, const int fd) noexcept {
  static_cast<void>(::epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr));
}

void wake_event(const int fd) noexcept {
  constexpr std::uint64_t value = 1;
  while (::write(fd, &value, sizeof(value)) < 0) {
    if (errno == EINTR) {
      continue;
    }
    return;
  }
}

void drain_counter(const int fd) noexcept {
  std::uint64_t value = 0;
  while (::read(fd, &value, sizeof(value)) < 0) {
    if (errno == EINTR) {
      continue;
    }
    return;
  }
}

class Worker final {
public:
  Worker(CacheEngine &engine, const std::size_t output_limit)
      : engine_(engine), output_limit_(output_limit), epoll_(create_epoll()),
        wakeup_(create_event()) {
    add_to_epoll(epoll_.get(), wakeup_.get(), EPOLLIN);
  }

  ~Worker() {
    request_stop();
    join();
  }

  Worker(const Worker &) = delete;
  Worker &operator=(const Worker &) = delete;

  void start() { thread_ = std::thread(&Worker::run, this); }

  void enqueue(UniqueFd socket) {
    {
      const std::lock_guard lock(pending_mutex_);
      if (stop_requested_.load(std::memory_order_relaxed)) {
        return;
      }
      pending_.push_back(std::move(socket));
    }
    wake_event(wakeup_.get());
  }

  void request_stop() noexcept {
    stop_requested_.store(true, std::memory_order_relaxed);
    wake_event(wakeup_.get());
  }

  void join() noexcept {
    if (thread_.joinable()) {
      thread_.join();
    }
  }

private:
  using ConnectionMap = std::unordered_map<int, std::unique_ptr<Connection>>;

  void run() noexcept {
    std::array<epoll_event, max_events> events{};
    while (!stop_requested_.load(std::memory_order_relaxed)) {
      const int count =
          ::epoll_wait(epoll_.get(), events.data(), events.size(), -1);
      if (count < 0) {
        if (errno == EINTR) {
          continue;
        }
        break;
      }

      bool wakeup_seen = false;
      for (int index = 0; index < count; ++index) {
        const auto event = events[static_cast<std::size_t>(index)];
        if (event.data.fd == wakeup_.get()) {
          drain_counter(wakeup_.get());
          wakeup_seen = true;
          continue;
        }
        handle_connection_event(event.data.fd, event.events);
      }
      if (wakeup_seen && !stop_requested_.load(std::memory_order_relaxed)) {
        install_pending();
      }
    }

    connections_.clear();
    const std::lock_guard lock(pending_mutex_);
    pending_.clear();
  }

  void install_pending() noexcept {
    std::vector<UniqueFd> pending;
    {
      const std::lock_guard lock(pending_mutex_);
      pending.swap(pending_);
    }

    for (auto &socket : pending) {
      try {
        const int fd = socket.get();
        auto connection = std::make_unique<Connection>(std::move(socket),
                                                       engine_, output_limit_);
        const auto [entry, inserted] =
            connections_.emplace(fd, std::move(connection));
        if (!inserted) {
          continue;
        }
        try {
          add_to_epoll(epoll_.get(), fd, entry->second->desired_events());
        } catch (const HighCacheError &) {
          connections_.erase(entry);
        }
      } catch (const std::exception &) {
      }
    }
  }

  void handle_connection_event(const int fd,
                               const std::uint32_t events) noexcept {
    const auto entry = connections_.find(fd);
    if (entry == connections_.end()) {
      return;
    }

    auto &connection = *entry->second;
    bool keep = (events & EPOLLERR) == 0;
    try {
      if (keep && (events & (EPOLLIN | EPOLLRDHUP | EPOLLHUP)) != 0) {
        keep = connection.handle_readable();
      }
      if (keep && (events & EPOLLOUT) != 0) {
        keep = connection.handle_writable();
      }
      if (keep && (events & (EPOLLRDHUP | EPOLLHUP)) != 0) {
        connection.note_peer_closed();
        keep = !connection.should_close();
      }
    } catch (const std::exception &) {
      keep = false;
    }

    if (keep && !connection.should_close()) {
      keep = modify_epoll(epoll_.get(), fd, connection.desired_events());
    }
    if (!keep || connection.should_close()) {
      remove_from_epoll(epoll_.get(), fd);
      connections_.erase(entry);
    }
  }

  CacheEngine &engine_;
  std::size_t output_limit_;
  UniqueFd epoll_;
  UniqueFd wakeup_;
  std::atomic<bool> stop_requested_{false};
  std::mutex pending_mutex_;
  std::vector<UniqueFd> pending_;
  ConnectionMap connections_;
  std::thread thread_;
};

struct ListenSocket final {
  UniqueFd socket;
  std::uint16_t port;
};

ServerOptions validate_options(ServerOptions options) {
  if (options.worker_threads == 0) {
    throw HighCacheError(ErrorCode::invalid_argument,
                         "server worker count must be greater than zero");
  }
  if (options.pending_output_limit < protocol::response_header_size) {
    throw HighCacheError(ErrorCode::invalid_argument,
                         "server output limit is too small");
  }
  return options;
}

ListenSocket create_listen_socket(const ServerOptions &options) {
  const int raw_socket = ::socket(
      AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_TCP);
  if (raw_socket < 0) {
    throw_network_error("socket failed");
  }
  UniqueFd socket(raw_socket);

  constexpr int enabled = 1;
  if (::setsockopt(socket.get(), SOL_SOCKET, SO_REUSEADDR, &enabled,
                   sizeof(enabled)) != 0) {
    throw_network_error("setsockopt SO_REUSEADDR failed");
  }

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(options.port);
  const int parse_result =
      ::inet_pton(AF_INET, options.host.c_str(), &address.sin_addr);
  if (parse_result != 1) {
    throw HighCacheError(ErrorCode::invalid_argument,
                         "server host must be an IPv4 address");
  }
  if (::bind(socket.get(), reinterpret_cast<const sockaddr *>(&address),
             sizeof(address)) != 0) {
    throw_network_error("bind failed");
  }
  if (::listen(socket.get(), SOMAXCONN) != 0) {
    throw_network_error("listen failed");
  }

  socklen_t address_length = sizeof(address);
  if (::getsockname(socket.get(), reinterpret_cast<sockaddr *>(&address),
                    &address_length) != 0) {
    throw_network_error("getsockname failed");
  }
  return {std::move(socket), ntohs(address.sin_port)};
}

} // namespace

class CacheServer::Impl final {
public:
  Impl(CacheEngine &engine, ServerOptions options)
      : engine_(engine), options_(validate_options(std::move(options))),
        listen_socket_(create_listen_socket(options_)), epoll_(create_epoll()),
        stop_event_(create_event()), timer_(create_timer()) {
    add_to_epoll(epoll_.get(), listen_socket_.socket.get(), EPOLLIN);
    add_to_epoll(epoll_.get(), stop_event_.get(), EPOLLIN);
    add_to_epoll(epoll_.get(), timer_.get(), EPOLLIN);
    workers_.reserve(options_.worker_threads);
    for (std::size_t index = 0; index < options_.worker_threads; ++index) {
      workers_.push_back(
          std::make_unique<Worker>(engine_, options_.pending_output_limit));
    }
  }

  ~Impl() {
    request_stop();
    join();
  }

  void start() {
    if (ever_started_.exchange(true, std::memory_order_relaxed)) {
      throw HighCacheError(ErrorCode::invalid_argument,
                           "cache server can only be started once");
    }

    itimerspec timer_specification{};
    timer_specification.it_value.tv_sec = 1;
    timer_specification.it_interval.tv_sec = 1;
    if (::timerfd_settime(timer_.get(), 0, &timer_specification, nullptr) !=
        0) {
      throw_network_error("timerfd_settime failed");
    }

    std::size_t started_workers = 0;
    try {
      for (auto &worker : workers_) {
        worker->start();
        ++started_workers;
      }
      running_.store(true, std::memory_order_release);
      acceptor_thread_ = std::thread(&Impl::run_acceptor, this);
    } catch (...) {
      running_.store(false, std::memory_order_release);
      for (std::size_t index = 0; index < started_workers; ++index) {
        workers_[index]->request_stop();
      }
      for (std::size_t index = 0; index < started_workers; ++index) {
        workers_[index]->join();
      }
      throw;
    }
  }

  void request_stop() noexcept {
    stop_requested_.store(true, std::memory_order_relaxed);
    wake_event(stop_event_.get());
  }

  void join() noexcept {
    if (acceptor_thread_.joinable()) {
      acceptor_thread_.join();
    }
    for (auto &worker : workers_) {
      worker->request_stop();
    }
    for (auto &worker : workers_) {
      worker->join();
    }
    running_.store(false, std::memory_order_release);
  }

  [[nodiscard]] const std::string &host() const noexcept {
    return options_.host;
  }

  [[nodiscard]] std::uint16_t port() const noexcept {
    return listen_socket_.port;
  }

  [[nodiscard]] std::size_t worker_count() const noexcept {
    return workers_.size();
  }

  [[nodiscard]] bool running() const noexcept {
    return running_.load(std::memory_order_acquire);
  }

private:
  void run_acceptor() noexcept {
    std::array<epoll_event, max_events> events{};
    while (!stop_requested_.load(std::memory_order_relaxed)) {
      const int count =
          ::epoll_wait(epoll_.get(), events.data(), events.size(), -1);
      if (count < 0) {
        if (errno == EINTR) {
          continue;
        }
        break;
      }

      for (int index = 0; index < count; ++index) {
        const int fd = events[static_cast<std::size_t>(index)].data.fd;
        if (fd == stop_event_.get()) {
          drain_counter(stop_event_.get());
          continue;
        }
        if (fd == timer_.get()) {
          handle_timer();
          continue;
        }
        if (fd == listen_socket_.socket.get()) {
          accept_connections();
        }
      }
    }
    running_.store(false, std::memory_order_release);
  }

  void handle_timer() noexcept {
    std::uint64_t expirations = 0;
    while (::read(timer_.get(), &expirations, sizeof(expirations)) < 0) {
      if (errno == EINTR) {
        continue;
      }
      return;
    }
    for (std::uint64_t tick = 0; tick < expirations; ++tick) {
      try {
        engine_.tick();
      } catch (const std::exception &) {
        return;
      }
    }
  }

  void accept_connections() noexcept {
    while (!stop_requested_.load(std::memory_order_relaxed)) {
      const int accepted = ::accept4(listen_socket_.socket.get(), nullptr,
                                     nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
      if (accepted >= 0) {
        auto &worker = workers_[next_worker_ % workers_.size()];
        ++next_worker_;
        try {
          worker->enqueue(UniqueFd(accepted));
        } catch (const std::exception &) {
        }
        continue;
      }
      if (errno == EINTR) {
        continue;
      }
      if (errno == ECONNABORTED || errno == EPROTO) {
        continue;
      }
      return;
    }
  }

  CacheEngine &engine_;
  ServerOptions options_;
  ListenSocket listen_socket_;
  UniqueFd epoll_;
  UniqueFd stop_event_;
  UniqueFd timer_;
  std::vector<std::unique_ptr<Worker>> workers_;
  std::thread acceptor_thread_;
  std::atomic<bool> ever_started_{false};
  std::atomic<bool> stop_requested_{false};
  std::atomic<bool> running_{false};
  std::size_t next_worker_{0};
};

CacheServer::CacheServer(CacheEngine &engine, ServerOptions options)
    : impl_(std::make_unique<Impl>(engine, std::move(options))) {}

CacheServer::~CacheServer() = default;

void CacheServer::start() { impl_->start(); }

void CacheServer::request_stop() noexcept { impl_->request_stop(); }

void CacheServer::join() noexcept { impl_->join(); }

const std::string &CacheServer::host() const noexcept { return impl_->host(); }

std::uint16_t CacheServer::port() const noexcept { return impl_->port(); }

std::size_t CacheServer::worker_count() const noexcept {
  return impl_->worker_count();
}

bool CacheServer::running() const noexcept { return impl_->running(); }

} // namespace highcache
