#include "highcache/protocol/protocol_codec.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace highcache::protocol {
namespace {

void write_u32(std::string &frame, const std::size_t offset,
               const std::uint32_t value) {
  frame[offset] = static_cast<char>(value >> 24U);
  frame[offset + 1] = static_cast<char>(value >> 16U);
  frame[offset + 2] = static_cast<char>(value >> 8U);
  frame[offset + 3] = static_cast<char>(value);
}

void write_u64(std::string &frame, const std::size_t offset,
               const std::uint64_t value) {
  write_u32(frame, offset, static_cast<std::uint32_t>(value >> 32U));
  write_u32(frame, offset + 4, static_cast<std::uint32_t>(value));
}

Request decode_complete_request(const std::string &frame) {
  Buffer input;
  input.append(frame);
  Request request;
  ProtocolError error;
  EXPECT_EQ(decode_request(input, request, error), DecodeResult::complete);
  EXPECT_TRUE(input.empty());
  return request;
}

TEST(ProtocolCodecTest, RoundTripsGetSetTtlDeleteAndBinaryValue) {
  const std::vector<Request> requests{
      {Opcode::get, 11, "get-key", {}, 0},
      {Opcode::set, 12, "set-key", std::string("a\0b", 3), 0},
      {Opcode::set, 13, "ttl-key", "value", 1500},
      {Opcode::erase, 14, "delete-key", {}, 0},
  };

  for (const auto &expected : requests) {
    const auto decoded = decode_complete_request(encode_request(expected));
    EXPECT_EQ(decoded.opcode, expected.opcode);
    EXPECT_EQ(decoded.request_id, expected.request_id);
    EXPECT_EQ(decoded.key, expected.key);
    EXPECT_EQ(decoded.value, expected.value);
    EXPECT_EQ(decoded.ttl_ms, expected.ttl_ms);
  }
}

TEST(ProtocolCodecTest, RoundTripsGetAndErrorResponsesWithRequestIds) {
  const std::vector<Response> responses{
      {101, WireStatus::ok, std::string("x\0y", 3)},
      {102, WireStatus::not_found, {}},
      {103, WireStatus::protocol_error, "invalid frame"},
  };

  for (const auto &expected : responses) {
    Buffer input;
    input.append(encode_response(expected));
    Response response;
    ProtocolError error;
    ASSERT_EQ(decode_response(input, response, error), DecodeResult::complete);
    EXPECT_EQ(response.request_id, expected.request_id);
    EXPECT_EQ(response.status, expected.status);
    EXPECT_EQ(response.value, expected.value);
  }
}

TEST(ProtocolCodecTest, ReassemblesInputOneByteAtATime) {
  const Request expected{Opcode::set, 0x0102030405060708ULL, "key", "value",
                         2500};
  const auto frame = encode_request(expected);
  Buffer input;
  Request request;
  ProtocolError error;

  for (std::size_t index = 0; index < frame.size(); ++index) {
    input.append(frame.data() + index, 1);
    const auto result = decode_request(input, request, error);
    EXPECT_EQ(result, index + 1 == frame.size() ? DecodeResult::complete
                                                : DecodeResult::incomplete);
  }

  EXPECT_EQ(request.request_id, expected.request_id);
  EXPECT_EQ(request.key, expected.key);
  EXPECT_EQ(request.value, expected.value);
}

TEST(ProtocolCodecTest, PreservesPartialBodyUntilComplete) {
  const auto frame = encode_request({Opcode::set, 55, "partial", "body", 0});
  Buffer input;
  input.append(frame.data(), request_header_size + 2);
  Request request;
  ProtocolError error;

  EXPECT_EQ(decode_request(input, request, error), DecodeResult::incomplete);
  const auto buffered = input.readable_bytes();
  input.append(frame.data() + buffered, frame.size() - buffered);
  EXPECT_EQ(decode_request(input, request, error), DecodeResult::complete);
  EXPECT_EQ(request.key, "partial");
  EXPECT_EQ(request.value, "body");
}

TEST(ProtocolCodecTest, DecodesStickyRequestsAndLeavesPartialRemainder) {
  const auto first = encode_request({Opcode::set, 1, "a", "1", 0});
  const auto second = encode_request({Opcode::get, 2, "a", {}, 0});
  const auto third = encode_request({Opcode::erase, 3, "a", {}, 0});
  const auto partial = encode_request({Opcode::get, 4, "later", {}, 0});
  Buffer input;
  input.append(first + second + third + partial.substr(0, 7));
  ProtocolError error;

  for (const auto expected_id : {1U, 2U, 3U}) {
    Request request;
    ASSERT_EQ(decode_request(input, request, error), DecodeResult::complete);
    EXPECT_EQ(request.request_id, expected_id);
  }
  Request request;
  EXPECT_EQ(decode_request(input, request, error), DecodeResult::incomplete);
  EXPECT_EQ(input.readable_bytes(), 7U);
}

class ProtocolMalformedTest
    : public ::testing::TestWithParam<std::string (*)(void)> {};

std::string invalid_magic() {
  auto frame = encode_request({Opcode::get, 91, "key", {}, 0});
  frame[0] ^= 0x01;
  return frame;
}

std::string invalid_version() {
  auto frame = encode_request({Opcode::get, 91, "key", {}, 0});
  frame[4] = 2;
  return frame;
}

std::string invalid_opcode() {
  auto frame = encode_request({Opcode::get, 91, "key", {}, 0});
  frame[5] = 99;
  return frame;
}

std::string invalid_reserved() {
  auto frame = encode_request({Opcode::get, 91, "key", {}, 0});
  frame[7] = 1;
  return frame;
}

std::string oversized_key() {
  auto frame = encode_request({Opcode::get, 91, "key", {}, 0});
  write_u32(frame, 16, static_cast<std::uint32_t>(Cache::max_key_length + 1));
  return frame;
}

std::string oversized_value() {
  auto frame = encode_request({Opcode::set, 91, "key", {}, 0});
  write_u32(frame, 20, static_cast<std::uint32_t>(Cache::max_value_length + 1));
  return frame;
}

std::string arithmetic_safety() {
  auto frame = encode_request({Opcode::set, 91, "key", {}, 0});
  write_u32(frame, 16, std::numeric_limits<std::uint32_t>::max());
  write_u32(frame, 20, std::numeric_limits<std::uint32_t>::max());
  return frame;
}

std::string get_with_value() {
  auto frame = encode_request({Opcode::set, 91, "key", "value", 0});
  frame[5] = static_cast<char>(Opcode::get);
  return frame;
}

std::string delete_with_ttl() {
  auto frame = encode_request({Opcode::erase, 91, "key", {}, 0});
  write_u64(frame, 24, 1);
  return frame;
}

std::string ttl_out_of_range() {
  auto frame = encode_request({Opcode::set, 91, "key", {}, 0});
  write_u64(
      frame, 24,
      static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1);
  return frame;
}

TEST_P(ProtocolMalformedTest, RejectsHeaderBeforeReadingAnUntrustedBody) {
  Buffer input;
  input.append(GetParam()());
  Request request;
  ProtocolError error;

  EXPECT_EQ(decode_request(input, request, error), DecodeResult::error);
  EXPECT_TRUE(error.has_request_id);
  EXPECT_EQ(error.request_id, 91U);
  EXPECT_FALSE(error.message.empty());
}

INSTANTIATE_TEST_SUITE_P(InvalidFrames, ProtocolMalformedTest,
                         ::testing::Values(&invalid_magic, &invalid_version,
                                           &invalid_opcode, &invalid_reserved,
                                           &oversized_key, &oversized_value,
                                           &arithmetic_safety, &get_with_value,
                                           &delete_with_ttl,
                                           &ttl_out_of_range));

TEST(ProtocolCodecTest, MapsEveryCacheStatusToStableWireStatus) {
  EXPECT_EQ(to_wire_status(CacheStatus::ok), WireStatus::ok);
  EXPECT_EQ(to_wire_status(CacheStatus::not_found), WireStatus::not_found);
  EXPECT_EQ(to_wire_status(CacheStatus::invalid_key), WireStatus::invalid_key);
  EXPECT_EQ(to_wire_status(CacheStatus::key_too_large),
            WireStatus::key_too_large);
  EXPECT_EQ(to_wire_status(CacheStatus::value_too_large),
            WireStatus::value_too_large);
  EXPECT_EQ(to_wire_status(CacheStatus::item_too_large),
            WireStatus::item_too_large);
  EXPECT_EQ(to_wire_status(CacheStatus::invalid_ttl), WireStatus::invalid_ttl);
}

TEST(ProtocolCodecTest, EncoderRejectsUnknownEnumValues) {
  EXPECT_THROW(encode_request({static_cast<Opcode>(99), 1, "key", {}, 0}),
               std::invalid_argument);
  EXPECT_THROW(encode_response({1, static_cast<WireStatus>(99), {}}),
               std::invalid_argument);
}

} // namespace
} // namespace highcache::protocol
