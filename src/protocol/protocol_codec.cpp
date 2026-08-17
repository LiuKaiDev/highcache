#include "highcache/protocol/protocol_codec.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>

namespace highcache::protocol {
namespace {

void append_u8(std::string &output, const std::uint8_t value) {
  output.push_back(static_cast<char>(value));
}

void append_u16(std::string &output, const std::uint16_t value) {
  append_u8(output, static_cast<std::uint8_t>(value >> 8U));
  append_u8(output, static_cast<std::uint8_t>(value));
}

void append_u32(std::string &output, const std::uint32_t value) {
  append_u8(output, static_cast<std::uint8_t>(value >> 24U));
  append_u8(output, static_cast<std::uint8_t>(value >> 16U));
  append_u8(output, static_cast<std::uint8_t>(value >> 8U));
  append_u8(output, static_cast<std::uint8_t>(value));
}

void append_u64(std::string &output, const std::uint64_t value) {
  append_u32(output, static_cast<std::uint32_t>(value >> 32U));
  append_u32(output, static_cast<std::uint32_t>(value));
}

std::uint8_t read_u8(const char *const data) {
  return static_cast<std::uint8_t>(static_cast<unsigned char>(*data));
}

std::uint16_t read_u16(const char *const data) {
  return static_cast<std::uint16_t>(
      (static_cast<std::uint16_t>(read_u8(data)) << 8U) | read_u8(data + 1));
}

std::uint32_t read_u32(const char *const data) {
  return (static_cast<std::uint32_t>(read_u8(data)) << 24U) |
         (static_cast<std::uint32_t>(read_u8(data + 1)) << 16U) |
         (static_cast<std::uint32_t>(read_u8(data + 2)) << 8U) |
         static_cast<std::uint32_t>(read_u8(data + 3));
}

std::uint64_t read_u64(const char *const data) {
  return (static_cast<std::uint64_t>(read_u32(data)) << 32U) |
         static_cast<std::uint64_t>(read_u32(data + 4));
}

DecodeResult fail(ProtocolError &error, const std::uint64_t request_id,
                  std::string message) {
  error.has_request_id = true;
  error.request_id = request_id;
  error.message = std::move(message);
  return DecodeResult::error;
}

bool is_valid_status(const std::uint8_t value) {
  return value <= static_cast<std::uint8_t>(WireStatus::internal_error);
}

bool is_valid_opcode(const Opcode opcode) {
  return opcode == Opcode::get || opcode == Opcode::set ||
         opcode == Opcode::erase;
}

} // namespace

std::string encode_request(const Request &request) {
  if (!is_valid_opcode(request.opcode)) {
    throw std::invalid_argument("request has unknown opcode");
  }
  if (request.key.size() > Cache::max_key_length ||
      request.value.size() > Cache::max_value_length) {
    throw std::invalid_argument("request exceeds protocol length limit");
  }
  if ((request.opcode == Opcode::get || request.opcode == Opcode::erase) &&
      (!request.value.empty() || request.ttl_ms != 0)) {
    throw std::invalid_argument("GET and DELETE cannot carry value or TTL");
  }
  if (request.ttl_ms >
      static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    throw std::invalid_argument("request TTL exceeds signed millisecond range");
  }

  std::string output;
  output.reserve(request_header_size + request.key.size() +
                 request.value.size());
  append_u32(output, magic);
  append_u8(output, version);
  append_u8(output, static_cast<std::uint8_t>(request.opcode));
  append_u16(output, 0);
  append_u64(output, request.request_id);
  append_u32(output, static_cast<std::uint32_t>(request.key.size()));
  append_u32(output, static_cast<std::uint32_t>(request.value.size()));
  append_u64(output, request.ttl_ms);
  output.append(request.key);
  output.append(request.value);
  return output;
}

std::string encode_response(const Response &response) {
  if (!is_valid_status(static_cast<std::uint8_t>(response.status))) {
    throw std::invalid_argument("response has unknown status");
  }
  if (response.value.size() > Cache::max_value_length) {
    throw std::invalid_argument("response exceeds protocol length limit");
  }

  std::string output;
  output.reserve(response_header_size + response.value.size());
  append_u32(output, magic);
  append_u8(output, version);
  append_u8(output, static_cast<std::uint8_t>(response.status));
  append_u16(output, 0);
  append_u64(output, response.request_id);
  append_u32(output, static_cast<std::uint32_t>(response.value.size()));
  output.append(response.value);
  return output;
}

DecodeResult decode_request(Buffer &input, Request &request,
                            ProtocolError &error) {
  if (input.readable_bytes() < request_header_size) {
    return DecodeResult::incomplete;
  }

  const char *const header = input.data();
  const auto request_id = read_u64(header + 8);
  if (read_u32(header) != magic) {
    return fail(error, request_id, "invalid request magic");
  }
  if (read_u8(header + 4) != version) {
    return fail(error, request_id, "unsupported protocol version");
  }
  if (read_u16(header + 6) != 0) {
    return fail(error, request_id, "request reserved field must be zero");
  }

  const auto opcode_value = read_u8(header + 5);
  if (opcode_value < static_cast<std::uint8_t>(Opcode::get) ||
      opcode_value > static_cast<std::uint8_t>(Opcode::erase)) {
    return fail(error, request_id, "unknown request opcode");
  }

  const auto key_length = static_cast<std::size_t>(read_u32(header + 16));
  const auto value_length = static_cast<std::size_t>(read_u32(header + 20));
  const auto ttl_ms = read_u64(header + 24);
  if (key_length > Cache::max_key_length) {
    return fail(error, request_id, "request key length exceeds maximum");
  }
  if (value_length > Cache::max_value_length) {
    return fail(error, request_id, "request value length exceeds maximum");
  }
  if (key_length > max_request_frame_size - request_header_size ||
      value_length >
          max_request_frame_size - request_header_size - key_length) {
    return fail(error, request_id, "request frame length overflow");
  }

  const auto opcode = static_cast<Opcode>(opcode_value);
  if ((opcode == Opcode::get || opcode == Opcode::erase) && value_length != 0) {
    return fail(error, request_id, "GET and DELETE cannot carry a value");
  }
  if ((opcode == Opcode::get || opcode == Opcode::erase) && ttl_ms != 0) {
    return fail(error, request_id, "GET and DELETE cannot carry a TTL");
  }
  if (ttl_ms >
      static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    return fail(error, request_id, "request TTL exceeds supported range");
  }

  const auto frame_length = request_header_size + key_length + value_length;
  if (input.readable_bytes() < frame_length) {
    return DecodeResult::incomplete;
  }

  request.opcode = opcode;
  request.request_id = request_id;
  request.key.assign(header + request_header_size, key_length);
  request.value.assign(header + request_header_size + key_length, value_length);
  request.ttl_ms = ttl_ms;
  input.retrieve(frame_length);
  error = {};
  return DecodeResult::complete;
}

DecodeResult decode_response(Buffer &input, Response &response,
                             ProtocolError &error) {
  if (input.readable_bytes() < response_header_size) {
    return DecodeResult::incomplete;
  }

  const char *const header = input.data();
  const auto request_id = read_u64(header + 8);
  if (read_u32(header) != magic) {
    return fail(error, request_id, "invalid response magic");
  }
  if (read_u8(header + 4) != version) {
    return fail(error, request_id, "unsupported response version");
  }
  if (read_u16(header + 6) != 0) {
    return fail(error, request_id, "response reserved field must be zero");
  }
  const auto status_value = read_u8(header + 5);
  if (!is_valid_status(status_value)) {
    return fail(error, request_id, "unknown response status");
  }

  const auto value_length = static_cast<std::size_t>(read_u32(header + 16));
  if (value_length > Cache::max_value_length) {
    return fail(error, request_id, "response value length exceeds maximum");
  }
  const auto frame_length = response_header_size + value_length;
  if (input.readable_bytes() < frame_length) {
    return DecodeResult::incomplete;
  }

  response.request_id = request_id;
  response.status = static_cast<WireStatus>(status_value);
  response.value.assign(header + response_header_size, value_length);
  input.retrieve(frame_length);
  error = {};
  return DecodeResult::complete;
}

WireStatus to_wire_status(const CacheStatus status) noexcept {
  switch (status) {
  case CacheStatus::ok:
    return WireStatus::ok;
  case CacheStatus::not_found:
    return WireStatus::not_found;
  case CacheStatus::invalid_key:
    return WireStatus::invalid_key;
  case CacheStatus::key_too_large:
    return WireStatus::key_too_large;
  case CacheStatus::value_too_large:
    return WireStatus::value_too_large;
  case CacheStatus::item_too_large:
    return WireStatus::item_too_large;
  case CacheStatus::invalid_ttl:
    return WireStatus::invalid_ttl;
  }
  return WireStatus::internal_error;
}

std::string_view to_string(const WireStatus status) noexcept {
  switch (status) {
  case WireStatus::ok:
    return "ok";
  case WireStatus::not_found:
    return "not_found";
  case WireStatus::invalid_key:
    return "invalid_key";
  case WireStatus::key_too_large:
    return "key_too_large";
  case WireStatus::value_too_large:
    return "value_too_large";
  case WireStatus::item_too_large:
    return "item_too_large";
  case WireStatus::invalid_ttl:
    return "invalid_ttl";
  case WireStatus::protocol_error:
    return "protocol_error";
  case WireStatus::internal_error:
    return "internal_error";
  }
  return "unknown";
}

} // namespace highcache::protocol
