#pragma once

#include "highcache/cache/cache_engine.h"
#include "highcache/net/buffer.h"
#include "highcache/net/unique_fd.h"
#include "highcache/protocol/protocol_codec.h"

#include <cstddef>
#include <cstdint>

namespace highcache {

class Connection final {
public:
  static constexpr std::size_t default_output_limit = 4 * 1024 * 1024;

  Connection(UniqueFd socket, CacheEngine &engine,
             std::size_t output_limit = default_output_limit);

  Connection(const Connection &) = delete;
  Connection &operator=(const Connection &) = delete;
  Connection(Connection &&) = delete;
  Connection &operator=(Connection &&) = delete;

  [[nodiscard]] int fd() const noexcept;
  [[nodiscard]] bool handle_readable();
  [[nodiscard]] bool handle_writable() noexcept;
  void note_peer_closed() noexcept;

  [[nodiscard]] bool wants_write() const noexcept;
  [[nodiscard]] bool should_close() const noexcept;
  [[nodiscard]] std::size_t pending_output_bytes() const noexcept;
  [[nodiscard]] std::uint32_t desired_events() const noexcept;

private:
  [[nodiscard]] bool process_input();
  [[nodiscard]] bool execute(const protocol::Request &request);
  [[nodiscard]] bool queue_response(const protocol::Response &response);
  [[nodiscard]] bool queue_protocol_error(const protocol::ProtocolError &error);
  void close_immediately() noexcept;

  UniqueFd socket_;
  CacheEngine &engine_;
  Buffer input_;
  Buffer output_;
  std::size_t output_limit_;
  bool peer_closed_{false};
  bool close_after_write_{false};
  bool close_immediately_{false};
};

} // namespace highcache
