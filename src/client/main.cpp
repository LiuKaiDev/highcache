#include "highcache/net/buffer.h"
#include "highcache/net/unique_fd.h"
#include "highcache/protocol/protocol_codec.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <array>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

std::uint64_t parse_unsigned(const std::string_view value) {
  std::uint64_t parsed = 0;
  const auto [end, error] =
      std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (error != std::errc{} || end != value.data() + value.size()) {
    throw std::invalid_argument("expected unsigned decimal integer");
  }
  return parsed;
}

highcache::UniqueFd connect_to(const std::string &host,
                               const std::uint16_t port) {
  const int raw_socket = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (raw_socket < 0) {
    throw std::runtime_error("socket failed");
  }
  highcache::UniqueFd socket(raw_socket);

  constexpr timeval timeout{5, 0};
  if (::setsockopt(socket.get(), SOL_SOCKET, SO_RCVTIMEO, &timeout,
                   sizeof(timeout)) != 0 ||
      ::setsockopt(socket.get(), SOL_SOCKET, SO_SNDTIMEO, &timeout,
                   sizeof(timeout)) != 0) {
    throw std::runtime_error("socket timeout setup failed");
  }

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  if (::inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1) {
    throw std::invalid_argument("host must be an IPv4 address");
  }
  if (::connect(socket.get(), reinterpret_cast<const sockaddr *>(&address),
                sizeof(address)) != 0) {
    throw std::runtime_error("connect failed");
  }
  return socket;
}

void send_all(const int fd, const std::string &frame) {
  std::size_t sent = 0;
  while (sent < frame.size()) {
    const auto result =
        ::send(fd, frame.data() + sent, frame.size() - sent, MSG_NOSIGNAL);
    if (result > 0) {
      sent += static_cast<std::size_t>(result);
      continue;
    }
    if (result < 0 && errno == EINTR) {
      continue;
    }
    throw std::runtime_error("send failed");
  }
}

highcache::protocol::Response read_response(const int fd) {
  highcache::Buffer input;
  while (true) {
    highcache::protocol::Response response;
    highcache::protocol::ProtocolError error;
    const auto decoded =
        highcache::protocol::decode_response(input, response, error);
    if (decoded == highcache::protocol::DecodeResult::complete) {
      return response;
    }
    if (decoded == highcache::protocol::DecodeResult::error) {
      throw std::runtime_error("invalid response: " + error.message);
    }

    std::array<char, 64 * 1024> bytes{};
    const auto result = ::recv(fd, bytes.data(), bytes.size(), 0);
    if (result > 0) {
      input.append(bytes.data(), static_cast<std::size_t>(result));
      continue;
    }
    if (result < 0 && errno == EINTR) {
      continue;
    }
    throw std::runtime_error("connection closed before response");
  }
}

highcache::protocol::Request parse_request(const int argc, char *argv[]) {
  if (argc < 5) {
    throw std::invalid_argument("usage: highcache_client HOST PORT "
                                "get|set|delete KEY [VALUE] [TTL_MS]");
  }

  highcache::protocol::Request request;
  request.request_id = 1;
  const std::string_view command(argv[3]);
  request.key = argv[4];
  if (command == "get" && argc == 5) {
    request.opcode = highcache::protocol::Opcode::get;
    return request;
  }
  if (command == "delete" && argc == 5) {
    request.opcode = highcache::protocol::Opcode::erase;
    return request;
  }
  if (command == "set" && (argc == 6 || argc == 7)) {
    request.opcode = highcache::protocol::Opcode::set;
    request.value = argv[5];
    if (argc == 7) {
      request.ttl_ms = parse_unsigned(argv[6]);
    }
    return request;
  }
  throw std::invalid_argument(
      "usage: highcache_client HOST PORT get|set|delete KEY [VALUE] [TTL_MS]");
}

} // namespace

int main(const int argc, char *argv[]) {
  try {
    const auto port_value = argc >= 3 ? parse_unsigned(argv[2]) : 0;
    if (port_value == 0 ||
        port_value > std::numeric_limits<std::uint16_t>::max()) {
      throw std::invalid_argument("port must be between 1 and 65535");
    }
    const auto request = parse_request(argc, argv);
    auto socket = connect_to(argv[1], static_cast<std::uint16_t>(port_value));
    send_all(socket.get(), highcache::protocol::encode_request(request));
    const auto response = read_response(socket.get());
    std::cout << highcache::protocol::to_string(response.status);
    if (!response.value.empty()) {
      std::cout << ' ';
      std::cout.write(response.value.data(),
                      static_cast<std::streamsize>(response.value.size()));
    }
    std::cout << '\n';
    return response.status == highcache::protocol::WireStatus::ok ? 0 : 2;
  } catch (const std::exception &error) {
    std::cerr << "highcache_client: " << error.what() << '\n';
    return 1;
  }
}
