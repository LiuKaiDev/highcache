#include "highcache/cache/cache_engine.h"
#include "highcache/net/connection.h"
#include "highcache/net/unique_fd.h"
#include "highcache/protocol/protocol_codec.h"

#include <gtest/gtest.h>

#include <sys/socket.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <string>
#include <utility>

namespace highcache {
namespace {

struct SocketPair final {
  UniqueFd connection_end;
  UniqueFd peer_end;
};

SocketPair make_socket_pair() {
  std::array<int, 2> descriptors{};
  if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0,
                   descriptors.data()) != 0) {
    return {};
  }
  return {UniqueFd(descriptors[0]), UniqueFd(descriptors[1])};
}

void send_all(const int fd, const std::string &data) {
  std::size_t sent = 0;
  while (sent < data.size()) {
    const auto result =
        ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
    ASSERT_GT(result, 0);
    sent += static_cast<std::size_t>(result);
  }
}

void drain_peer(const int fd, Buffer &received) {
  std::array<char, 64 * 1024> bytes{};
  while (true) {
    const auto result = ::recv(fd, bytes.data(), bytes.size(), 0);
    if (result > 0) {
      received.append(bytes.data(), static_cast<std::size_t>(result));
      continue;
    }
    if (result < 0 && errno == EINTR) {
      continue;
    }
    ASSERT_TRUE(result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
    return;
  }
}

protocol::Response decode_response(Buffer &input) {
  protocol::Response response;
  protocol::ProtocolError error;
  EXPECT_EQ(protocol::decode_response(input, response, error),
            protocol::DecodeResult::complete);
  return response;
}

TEST(ConnectionTest, PreservesPartialRequestAndReturnsBinaryResponse) {
  auto sockets = make_socket_pair();
  ASSERT_TRUE(sockets.connection_end.valid());
  CacheEngine engine(1024 * 1024, 4);
  Connection connection(std::move(sockets.connection_end), engine);
  const auto request = protocol::encode_request(
      {protocol::Opcode::set, 77, "key", std::string("a\0b", 3), 0});

  send_all(sockets.peer_end.get(), request.substr(0, 9));
  EXPECT_TRUE(connection.handle_readable());
  EXPECT_FALSE(connection.wants_write());

  send_all(sockets.peer_end.get(), request.substr(9));
  EXPECT_TRUE(connection.handle_readable());
  EXPECT_TRUE(connection.wants_write());
  EXPECT_TRUE(connection.handle_writable());

  Buffer received;
  drain_peer(sockets.peer_end.get(), received);
  const auto response = decode_response(received);
  EXPECT_EQ(response.request_id, 77U);
  EXPECT_EQ(response.status, protocol::WireStatus::ok);

  send_all(sockets.peer_end.get(),
           protocol::encode_request({protocol::Opcode::get, 78, "key", {}, 0}));
  EXPECT_TRUE(connection.handle_readable());
  EXPECT_TRUE(connection.handle_writable());
  drain_peer(sockets.peer_end.get(), received);
  const auto get_response = decode_response(received);
  EXPECT_EQ(get_response.request_id, 78U);
  EXPECT_EQ(get_response.value, std::string("a\0b", 3));
}

TEST(ConnectionTest, ClosesCleanlyWhenPeerEndsWithPartialFrame) {
  auto sockets = make_socket_pair();
  ASSERT_TRUE(sockets.connection_end.valid());
  CacheEngine engine;
  Connection connection(std::move(sockets.connection_end), engine);
  const auto request =
      protocol::encode_request({protocol::Opcode::get, 1, "partial", {}, 0});

  send_all(sockets.peer_end.get(), request.substr(0, 7));
  sockets.peer_end.reset();

  EXPECT_FALSE(connection.handle_readable());
  EXPECT_TRUE(connection.should_close());
}

TEST(ConnectionTest, RetainsUnwrittenBytesAcrossEagain) {
  auto sockets = make_socket_pair();
  ASSERT_TRUE(sockets.connection_end.valid());
  int send_buffer_size = 4096;
  ASSERT_EQ(::setsockopt(sockets.connection_end.get(), SOL_SOCKET, SO_SNDBUF,
                         &send_buffer_size, sizeof(send_buffer_size)),
            0);
  CacheEngine engine(8 * 1024 * 1024, 1);
  const std::string value(Cache::max_value_length, 'v');
  ASSERT_EQ(engine.set("large", value), CacheStatus::ok);
  Connection connection(std::move(sockets.connection_end), engine);

  std::string requests;
  for (std::uint64_t id = 1; id <= 3; ++id) {
    requests +=
        protocol::encode_request({protocol::Opcode::get, id, "large", {}, 0});
  }
  send_all(sockets.peer_end.get(), requests);
  ASSERT_TRUE(connection.handle_readable());
  ASSERT_GT(connection.pending_output_bytes(), 3 * Cache::max_value_length);
  EXPECT_TRUE(connection.handle_writable());
  EXPECT_TRUE(connection.wants_write());

  Buffer received;
  for (std::size_t iteration = 0; iteration < 10000 && connection.wants_write();
       ++iteration) {
    drain_peer(sockets.peer_end.get(), received);
    ASSERT_TRUE(connection.handle_writable());
  }
  drain_peer(sockets.peer_end.get(), received);
  ASSERT_FALSE(connection.wants_write());

  for (std::uint64_t id = 1; id <= 3; ++id) {
    const auto response = decode_response(received);
    EXPECT_EQ(response.request_id, id);
    EXPECT_EQ(response.status, protocol::WireStatus::ok);
    EXPECT_EQ(response.value, value);
  }
  EXPECT_TRUE(received.empty());
}

TEST(ConnectionTest, ClosesWhenPendingOutputWouldExceedLimit) {
  auto sockets = make_socket_pair();
  ASSERT_TRUE(sockets.connection_end.valid());
  CacheEngine engine(1024, 1);
  ASSERT_EQ(engine.set("key", std::string(200, 'x')), CacheStatus::ok);
  Connection connection(std::move(sockets.connection_end), engine, 100);
  send_all(sockets.peer_end.get(),
           protocol::encode_request({protocol::Opcode::get, 1, "key", {}, 0}));

  EXPECT_FALSE(connection.handle_readable());
  EXPECT_TRUE(connection.should_close());
}

TEST(ConnectionTest, SendsProtocolErrorThenCloses) {
  auto sockets = make_socket_pair();
  ASSERT_TRUE(sockets.connection_end.valid());
  CacheEngine engine;
  Connection connection(std::move(sockets.connection_end), engine);
  auto request =
      protocol::encode_request({protocol::Opcode::get, 1234, "key", {}, 0});
  request[0] ^= 1;
  send_all(sockets.peer_end.get(), request);

  EXPECT_TRUE(connection.handle_readable());
  EXPECT_TRUE(connection.wants_write());
  EXPECT_TRUE(connection.handle_writable() == false);
  EXPECT_TRUE(connection.should_close());

  Buffer received;
  drain_peer(sockets.peer_end.get(), received);
  const auto response = decode_response(received);
  EXPECT_EQ(response.request_id, 1234U);
  EXPECT_EQ(response.status, protocol::WireStatus::protocol_error);
}

} // namespace
} // namespace highcache
