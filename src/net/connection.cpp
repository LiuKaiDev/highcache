#include "highcache/net/connection.h"

#include <sys/epoll.h>
#include <sys/socket.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <string>
#include <utility>

namespace highcache {
namespace {

constexpr std::size_t read_chunk_size = 64 * 1024;
constexpr std::size_t max_input_buffer_size =
    protocol::max_request_frame_size + read_chunk_size;

} // namespace

Connection::Connection(UniqueFd socket, CacheEngine &engine,
                       const std::size_t output_limit)
    : socket_(std::move(socket)), engine_(engine), output_limit_(output_limit) {
}

int Connection::fd() const noexcept { return socket_.get(); }

bool Connection::handle_readable() {
  std::array<char, read_chunk_size> bytes;
  while (!close_after_write_ && !close_immediately_) {
    const auto result = ::recv(fd(), bytes.data(), bytes.size(), 0);
    if (result > 0) {
      const auto received = static_cast<std::size_t>(result);
      if (received > max_input_buffer_size - input_.readable_bytes()) {
        close_immediately();
        break;
      }
      input_.append(bytes.data(), received);
      if (!process_input()) {
        break;
      }
      continue;
    }

    if (result == 0) {
      note_peer_closed();
      break;
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      break;
    }

    close_immediately();
    break;
  }

  return !should_close();
}

bool Connection::handle_writable() noexcept {
  while (!output_.empty() && !close_immediately_) {
    const auto result =
        ::send(fd(), output_.data(), output_.readable_bytes(), MSG_NOSIGNAL);
    if (result > 0) {
      output_.retrieve(static_cast<std::size_t>(result));
      continue;
    }
    if (result < 0 && errno == EINTR) {
      continue;
    }
    if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      return true;
    }

    close_immediately();
    return false;
  }

  if (output_.empty() && (close_after_write_ || peer_closed_)) {
    close_immediately();
  }
  return !should_close();
}

void Connection::note_peer_closed() noexcept {
  peer_closed_ = true;
  if (output_.empty()) {
    close_immediately();
  }
}

bool Connection::wants_write() const noexcept { return !output_.empty(); }

bool Connection::should_close() const noexcept { return close_immediately_; }

std::size_t Connection::pending_output_bytes() const noexcept {
  return output_.readable_bytes();
}

std::uint32_t Connection::desired_events() const noexcept {
  if (should_close()) {
    return 0;
  }

  std::uint32_t events = EPOLLRDHUP;
  if (!close_after_write_ && !peer_closed_) {
    events |= EPOLLIN;
  }
  if (wants_write()) {
    events |= EPOLLOUT;
  }
  return events;
}

bool Connection::process_input() {
  while (!input_.empty()) {
    protocol::Request request;
    protocol::ProtocolError error;
    const auto result = protocol::decode_request(input_, request, error);
    if (result == protocol::DecodeResult::incomplete) {
      return true;
    }
    if (result == protocol::DecodeResult::error) {
      input_.retrieve_all();
      return queue_protocol_error(error);
    }
    if (!execute(request)) {
      return false;
    }
  }
  return true;
}

bool Connection::execute(const protocol::Request &request) {
  protocol::Response response;
  response.request_id = request.request_id;
  CacheStatus status = CacheStatus::not_found;

  try {
    switch (request.opcode) {
    case protocol::Opcode::get:
      status = engine_.get(request.key, response.value);
      if (status != CacheStatus::ok) {
        response.value.clear();
      }
      break;
    case protocol::Opcode::set:
      status = engine_.set(
          request.key, request.value,
          std::chrono::milliseconds{static_cast<std::int64_t>(request.ttl_ms)});
      break;
    case protocol::Opcode::erase:
      status = engine_.erase(request.key);
      break;
    }
    response.status = protocol::to_wire_status(status);
  } catch (const std::exception &) {
    response.status = protocol::WireStatus::internal_error;
    response.value.clear();
  }

  return queue_response(response);
}

bool Connection::queue_response(const protocol::Response &response) {
  const auto frame = protocol::encode_response(response);
  const auto pending = output_.readable_bytes();
  if (pending > output_limit_ || frame.size() > output_limit_ - pending) {
    close_immediately();
    return false;
  }
  output_.append(frame);
  return true;
}

bool Connection::queue_protocol_error(const protocol::ProtocolError &error) {
  protocol::Response response;
  response.request_id = error.has_request_id ? error.request_id : 0;
  response.status = protocol::WireStatus::protocol_error;
  response.value = error.message;
  if (!queue_response(response)) {
    return false;
  }
  close_after_write_ = true;
  return true;
}

void Connection::close_immediately() noexcept { close_immediately_ = true; }

} // namespace highcache
