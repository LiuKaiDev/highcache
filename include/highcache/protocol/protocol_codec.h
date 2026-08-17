#pragma once

#include "highcache/cache/cache.h"
#include "highcache/net/buffer.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace highcache::protocol {

constexpr std::uint32_t magic = 0x48434143U;
constexpr std::uint8_t version = 1;
constexpr std::size_t request_header_size = 32;
constexpr std::size_t response_header_size = 20;
constexpr std::size_t max_request_frame_size =
    request_header_size + Cache::max_key_length + Cache::max_value_length;
constexpr std::size_t max_response_frame_size =
    response_header_size + Cache::max_value_length;

enum class Opcode : std::uint8_t {
  get = 1,
  set = 2,
  erase = 3,
};

enum class WireStatus : std::uint8_t {
  ok = 0,
  not_found = 1,
  invalid_key = 2,
  key_too_large = 3,
  value_too_large = 4,
  item_too_large = 5,
  invalid_ttl = 6,
  protocol_error = 7,
  internal_error = 8,
};

struct Request final {
  Opcode opcode{Opcode::get};
  std::uint64_t request_id{0};
  std::string key;
  std::string value;
  std::uint64_t ttl_ms{0};
};

struct Response final {
  std::uint64_t request_id{0};
  WireStatus status{WireStatus::ok};
  std::string value;
};

enum class DecodeResult {
  incomplete,
  complete,
  error,
};

struct ProtocolError final {
  bool has_request_id{false};
  std::uint64_t request_id{0};
  std::string message;
};

[[nodiscard]] std::string encode_request(const Request &request);
[[nodiscard]] std::string encode_response(const Response &response);
[[nodiscard]] DecodeResult decode_request(Buffer &input, Request &request,
                                          ProtocolError &error);
[[nodiscard]] DecodeResult decode_response(Buffer &input, Response &response,
                                           ProtocolError &error);
[[nodiscard]] WireStatus to_wire_status(CacheStatus status) noexcept;
[[nodiscard]] std::string_view to_string(WireStatus status) noexcept;

} // namespace highcache::protocol
