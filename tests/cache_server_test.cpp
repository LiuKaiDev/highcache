#include "highcache/cache/cache_engine.h"
#include "highcache/common/error.h"
#include "highcache/net/cache_server.h"
#include "highcache/net/connection.h"
#include "highcache/protocol/protocol_codec.h"

#include "net_test_utils.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace highcache {
namespace {

using namespace std::chrono_literals;

class RunningServer final {
public:
  explicit RunningServer(const std::size_t worker_count = 4)
      : engine_(64 * 1024 * 1024, 64),
        server_(engine_, ServerOptions{"127.0.0.1", 0, worker_count,
                                       Connection::default_output_limit}) {
    server_.start();
  }

  ~RunningServer() {
    server_.request_stop();
    server_.join();
  }

  [[nodiscard]] test::ProtocolClient client() const {
    return test::ProtocolClient(server_.host(), server_.port());
  }

  [[nodiscard]] CacheServer &server() noexcept { return server_; }

private:
  CacheEngine engine_;
  CacheServer server_;
};

TEST(CacheServerTest, BindsEphemeralPortAndRejectsInvalidOptions) {
  CacheEngine engine;
  CacheServer server(engine,
                     {"127.0.0.1", 0, 2, Connection::default_output_limit});

  EXPECT_EQ(server.host(), "127.0.0.1");
  EXPECT_NE(server.port(), 0U);
  EXPECT_EQ(server.worker_count(), 2U);
  EXPECT_FALSE(server.running());

  EXPECT_THROW(CacheServer(engine, {"127.0.0.1", 0, 0,
                                    Connection::default_output_limit}),
               HighCacheError);
  EXPECT_THROW(
      CacheServer(engine, {"invalid", 0, 1, Connection::default_output_limit}),
      HighCacheError);
  EXPECT_THROW(CacheServer(engine, {"127.0.0.1", 0, 1, 0}), HighCacheError);
}

TEST(CacheServerIntegrationTest, SupportsRemoteCacheLifecycleAndBinaryValues) {
  RunningServer running;
  auto client = running.client();
  const std::string binary("a\0b\0c", 5);

  auto response = client.request({protocol::Opcode::set, 1, "key", binary, 0});
  EXPECT_EQ(response.request_id, 1U);
  EXPECT_EQ(response.status, protocol::WireStatus::ok);
  EXPECT_TRUE(response.value.empty());

  response = client.request({protocol::Opcode::get, 2, "key", {}, 0});
  EXPECT_EQ(response.request_id, 2U);
  EXPECT_EQ(response.status, protocol::WireStatus::ok);
  EXPECT_EQ(response.value, binary);

  ASSERT_EQ(client.request({protocol::Opcode::set, 3, "key", "new", 0}).status,
            protocol::WireStatus::ok);
  EXPECT_EQ(client.request({protocol::Opcode::get, 4, "key", {}, 0}).value,
            "new");
  EXPECT_EQ(client.request({protocol::Opcode::erase, 5, "key", {}, 0}).status,
            protocol::WireStatus::ok);
  EXPECT_EQ(client.request({protocol::Opcode::get, 6, "key", {}, 0}).status,
            protocol::WireStatus::not_found);
}

TEST(CacheServerIntegrationTest, ReassemblesOneByteFragments) {
  RunningServer running;
  auto client = running.client();
  const auto frame = protocol::encode_request(
      {protocol::Opcode::set, 71, "fragmented", "body", 0});

  client.send_chunks(frame, 1);
  const auto response = client.read_response();

  EXPECT_EQ(response.request_id, 71U);
  EXPECT_EQ(response.status, protocol::WireStatus::ok);
  EXPECT_EQ(
      client.request({protocol::Opcode::get, 72, "fragmented", {}, 0}).value,
      "body");
}

TEST(CacheServerIntegrationTest, ParsesStickyPipelinedFramesInOrder) {
  RunningServer running;
  auto client = running.client();
  const auto pipeline =
      protocol::encode_request({protocol::Opcode::set, 101, "pipe", "v", 0}) +
      protocol::encode_request({protocol::Opcode::get, 102, "pipe", {}, 0}) +
      protocol::encode_request({protocol::Opcode::erase, 103, "pipe", {}, 0});

  client.send_raw(pipeline);
  const auto first = client.read_response();
  const auto second = client.read_response();
  const auto third = client.read_response();

  EXPECT_EQ(first.request_id, 101U);
  EXPECT_EQ(first.status, protocol::WireStatus::ok);
  EXPECT_EQ(second.request_id, 102U);
  EXPECT_EQ(second.value, "v");
  EXPECT_EQ(third.request_id, 103U);
  EXPECT_EQ(third.status, protocol::WireStatus::ok);
}

TEST(CacheServerIntegrationTest, SharesOneEngineAcrossWorkers) {
  RunningServer running(2);
  auto first_worker_client = running.client();
  auto second_worker_client = running.client();

  EXPECT_EQ(first_worker_client
                .request({protocol::Opcode::set, 1, "shared", "visible", 0})
                .status,
            protocol::WireStatus::ok);
  const auto response =
      second_worker_client.request({protocol::Opcode::get, 2, "shared", {}, 0});
  EXPECT_EQ(response.status, protocol::WireStatus::ok);
  EXPECT_EQ(response.value, "visible");
}

TEST(CacheServerIntegrationTest, SingleTimerOwnerExpiresProtocolTtl) {
  RunningServer running(8);
  auto client = running.client();

  ASSERT_EQ(
      client.request({protocol::Opcode::set, 1, "ttl", "value", 1}).status,
      protocol::WireStatus::ok);

  protocol::Response response;
  for (std::size_t attempt = 0; attempt < 150; ++attempt) {
    response =
        client.request({protocol::Opcode::get, 2 + attempt, "ttl", {}, 0});
    if (response.status == protocol::WireStatus::not_found) {
      break;
    }
    std::this_thread::sleep_for(20ms);
  }
  EXPECT_EQ(response.status, protocol::WireStatus::not_found);
}

TEST(CacheServerIntegrationTest, SurvivesAbnormalClientDisconnects) {
  RunningServer running;
  {
    auto client = running.client();
    const auto partial = protocol::encode_request(
        {protocol::Opcode::set, 1, "partial", "value", 0});
    client.send_raw(std::string_view(partial).substr(0, 11));
  }
  {
    auto client = running.client();
    client.send_raw(protocol::encode_request(
        {protocol::Opcode::set, 2, "unread", "value", 0}));
  }

  auto healthy = running.client();
  EXPECT_EQ(
      healthy.request({protocol::Opcode::set, 3, "healthy", "value", 0}).status,
      protocol::WireStatus::ok);
  EXPECT_EQ(healthy.request({protocol::Opcode::get, 4, "healthy", {}, 0}).value,
            "value");
}

TEST(CacheServerIntegrationTest,
     ReturnsProtocolErrorBeforeClosingMalformedPeer) {
  RunningServer running;
  auto client = running.client();
  auto frame =
      protocol::encode_request({protocol::Opcode::get, 444, "key", {}, 0});
  frame[4] = 99;

  client.send_raw(frame);
  const auto response = client.read_response();

  EXPECT_EQ(response.request_id, 444U);
  EXPECT_EQ(response.status, protocol::WireStatus::protocol_error);
  EXPECT_FALSE(response.value.empty());
  EXPECT_TRUE(client.peer_closed());
}

TEST(CacheServerIntegrationTest, KeepsConnectionOpenAfterBusinessError) {
  RunningServer running;
  auto client = running.client();

  const auto invalid = client.request({protocol::Opcode::get, 501, {}, {}, 0});
  EXPECT_EQ(invalid.request_id, 501U);
  EXPECT_EQ(invalid.status, protocol::WireStatus::invalid_key);

  const auto valid =
      client.request({protocol::Opcode::set, 502, "still-open", "value", 0});
  EXPECT_EQ(valid.request_id, 502U);
  EXPECT_EQ(valid.status, protocol::WireStatus::ok);
}

TEST(CacheServerIntegrationTest,
     HandlesOneHundredTwentyEightConcurrentClients) {
  constexpr std::size_t client_count = 128;
  RunningServer running(8);
  std::atomic<bool> success{true};
  std::vector<std::thread> clients;
  clients.reserve(client_count);

  for (std::size_t index = 0; index < client_count; ++index) {
    clients.emplace_back([&, index] {
      try {
        auto client = running.client();
        const auto key = "concurrent-" + std::to_string(index);
        const auto value = "value-" + std::to_string(index);
        const auto ttl = index % 8 == 0 ? 5000U : 0U;
        const auto base_id = static_cast<std::uint64_t>(index * 10);
        const auto set = client.request(
            {protocol::Opcode::set, base_id + 1, key, value, ttl});
        const auto get =
            client.request({protocol::Opcode::get, base_id + 2, key, {}, 0});
        const auto erase =
            client.request({protocol::Opcode::erase, base_id + 3, key, {}, 0});
        const auto miss =
            client.request({protocol::Opcode::get, base_id + 4, key, {}, 0});
        if (set.status != protocol::WireStatus::ok ||
            get.status != protocol::WireStatus::ok || get.value != value ||
            erase.status != protocol::WireStatus::ok ||
            miss.status != protocol::WireStatus::not_found ||
            miss.request_id != base_id + 4) {
          success.store(false, std::memory_order_relaxed);
        }
      } catch (const std::exception &) {
        success.store(false, std::memory_order_relaxed);
      }
    });
  }

  for (auto &client : clients) {
    client.join();
  }
  EXPECT_TRUE(success.load(std::memory_order_relaxed));
}

TEST(CacheServerIntegrationTest, StopsAndJoinsWithActiveConnections) {
  CacheEngine engine;
  CacheServer server(engine,
                     {"127.0.0.1", 0, 4, Connection::default_output_limit});
  server.start();
  std::vector<std::unique_ptr<test::ProtocolClient>> clients;
  for (std::size_t index = 0; index < 16; ++index) {
    clients.push_back(
        std::make_unique<test::ProtocolClient>(server.host(), server.port()));
  }

  server.request_stop();
  server.join();

  EXPECT_FALSE(server.running());
}

} // namespace
} // namespace highcache
