#pragma once

#include "highcache/net/buffer.h"
#include "highcache/net/unique_fd.h"
#include "highcache/protocol/protocol_codec.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace highcache::test {

class ProtocolClient final {
public:
  ProtocolClient(const std::string &host, const std::uint16_t port) {
    const int raw_socket = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (raw_socket < 0) {
      throw std::runtime_error("client socket failed");
    }
    socket_.reset(raw_socket);

    constexpr timeval timeout{5, 0};
    if (::setsockopt(socket_.get(), SOL_SOCKET, SO_RCVTIMEO, &timeout,
                     sizeof(timeout)) != 0 ||
        ::setsockopt(socket_.get(), SOL_SOCKET, SO_SNDTIMEO, &timeout,
                     sizeof(timeout)) != 0) {
      throw std::runtime_error("client timeout setup failed");
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1) {
      throw std::runtime_error("invalid client IPv4 address");
    }
    if (::connect(socket_.get(), reinterpret_cast<const sockaddr *>(&address),
                  sizeof(address)) != 0) {
      throw std::runtime_error("client connect failed");
    }
  }

  void send_raw(const std::string_view bytes) {
    std::size_t sent = 0;
    while (sent < bytes.size()) {
      const auto result = ::send(socket_.get(), bytes.data() + sent,
                                 bytes.size() - sent, MSG_NOSIGNAL);
      if (result > 0) {
        sent += static_cast<std::size_t>(result);
        continue;
      }
      if (result < 0 && errno == EINTR) {
        continue;
      }
      throw std::runtime_error("client send failed");
    }
  }

  void send_chunks(const std::string_view bytes, const std::size_t chunk_size) {
    if (chunk_size == 0) {
      throw std::invalid_argument("chunk size must be non-zero");
    }
    for (std::size_t offset = 0; offset < bytes.size(); offset += chunk_size) {
      const auto length = std::min(chunk_size, bytes.size() - offset);
      send_raw(bytes.substr(offset, length));
    }
  }

  [[nodiscard]] protocol::Response request(const protocol::Request &request) {
    send_raw(protocol::encode_request(request));
    return read_response();
  }

  [[nodiscard]] protocol::Response read_response() {
    while (true) {
      protocol::Response response;
      protocol::ProtocolError error;
      const auto decoded = protocol::decode_response(input_, response, error);
      if (decoded == protocol::DecodeResult::complete) {
        return response;
      }
      if (decoded == protocol::DecodeResult::error) {
        throw std::runtime_error("invalid server response: " + error.message);
      }

      std::array<char, 64 * 1024> bytes{};
      const auto result = ::recv(socket_.get(), bytes.data(), bytes.size(), 0);
      if (result > 0) {
        input_.append(bytes.data(), static_cast<std::size_t>(result));
        continue;
      }
      if (result < 0 && errno == EINTR) {
        continue;
      }
      throw std::runtime_error("server closed before complete response");
    }
  }

  void shutdown_write() noexcept {
    static_cast<void>(::shutdown(socket_.get(), SHUT_WR));
  }

  [[nodiscard]] bool peer_closed() {
    char byte = 0;
    while (true) {
      const auto result = ::recv(socket_.get(), &byte, sizeof(byte), 0);
      if (result == 0) {
        return true;
      }
      if (result < 0 && errno == EINTR) {
        continue;
      }
      return false;
    }
  }

private:
  UniqueFd socket_;
  Buffer input_;
};

} // namespace highcache::test
