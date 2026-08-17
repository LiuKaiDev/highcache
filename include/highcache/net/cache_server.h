#pragma once

#include "highcache/cache/cache_engine.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace highcache {

struct ServerOptions final {
  std::string host{"127.0.0.1"};
  std::uint16_t port{11211};
  std::size_t worker_threads{4};
  std::size_t pending_output_limit{4 * 1024 * 1024};
};

class CacheServer final {
public:
  CacheServer(CacheEngine &engine, ServerOptions options = {});
  ~CacheServer();

  CacheServer(const CacheServer &) = delete;
  CacheServer &operator=(const CacheServer &) = delete;
  CacheServer(CacheServer &&) = delete;
  CacheServer &operator=(CacheServer &&) = delete;

  void start();
  void request_stop() noexcept;
  void join() noexcept;

  [[nodiscard]] const std::string &host() const noexcept;
  [[nodiscard]] std::uint16_t port() const noexcept;
  [[nodiscard]] std::size_t worker_count() const noexcept;
  [[nodiscard]] bool running() const noexcept;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace highcache
