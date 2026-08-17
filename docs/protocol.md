# HighCache Binary Protocol

HighCache protocol version 1 is a request/response protocol over a persistent TCP byte stream. All integers use unsigned fixed-width fields encoded in big-endian byte order. Headers are serialized field by field; they are not C++ object representations. Keys and values are raw length-delimited bytes and may contain null bytes.

## Request Frame

The request header is exactly 32 bytes, followed by `key_length` key bytes and then `value_length` value bytes.

| Offset | Field | Size | Byte order | Meaning |
| ---: | --- | ---: | --- | --- |
| 0 | `magic` | 4 | big-endian | `0x48434143` (`HCAC`) |
| 4 | `version` | 1 | n/a | `1` |
| 5 | `opcode` | 1 | n/a | `1` GET, `2` SET, `3` DELETE |
| 6 | `reserved` | 2 | big-endian | must be zero |
| 8 | `request_id` | 8 | big-endian | opaque client-selected correlation ID |
| 16 | `key_length` | 4 | big-endian | key body length in bytes |
| 20 | `value_length` | 4 | big-endian | value body length in bytes |
| 24 | `ttl_ms` | 8 | big-endian | SET lifetime in milliseconds |
| 32 | key | `key_length` | n/a | raw key bytes |
| variable | value | `value_length` | n/a | raw value bytes |

GET and DELETE require `value_length == 0` and `ttl_ms == 0`. SET carries its key and value. For SET, `ttl_ms == 0` creates a persistent item; a positive value is forwarded to the cache TTL API and rounds up to the next one-second cache tick. TTL values above `INT64_MAX` are rejected as protocol errors.

`request_id` has no server-side sequencing meaning. The server copies it into the corresponding response, allowing multiple requests to be pipelined without assuming only one outstanding request.

## Response Frame

The response header is exactly 20 bytes, followed by `value_length` value bytes.

| Offset | Field | Size | Byte order | Meaning |
| ---: | --- | ---: | --- | --- |
| 0 | `magic` | 4 | big-endian | `0x48434143` (`HCAC`) |
| 4 | `version` | 1 | n/a | `1` |
| 5 | `status` | 1 | n/a | stable status code from the table below |
| 6 | `reserved` | 2 | big-endian | zero |
| 8 | `request_id` | 8 | big-endian | copied from the request |
| 16 | `value_length` | 4 | big-endian | response body length in bytes |
| 20 | value | `value_length` | n/a | GET value or protocol-error detail |

A successful GET returns the cached bytes. Successful SET and DELETE responses normally have empty bodies. A missing GET or DELETE returns `not_found` with an empty body.

## Opcodes

| Value | Name | Body | TTL |
| ---: | --- | --- | --- |
| 1 | GET | key only | must be zero |
| 2 | SET | key followed by value | zero or positive milliseconds |
| 3 | DELETE | key only | must be zero |

No other operations are defined in version 1.

## Status Codes

| Value | Name | Meaning |
| ---: | --- | --- |
| 0 | `ok` | operation completed successfully |
| 1 | `not_found` | key does not exist |
| 2 | `invalid_key` | key is empty |
| 3 | `key_too_large` | key exceeds the cache limit |
| 4 | `value_too_large` | value exceeds the cache limit |
| 5 | `item_too_large` | item cannot fit within cache capacity |
| 6 | `invalid_ttl` | TTL is not accepted by the cache |
| 7 | `protocol_error` | frame violates the wire format |
| 8 | `internal_error` | request execution raised an unexpected exception |

Statuses 0 through 6 are normal cache results. The connection remains available after them. Status 7 is a fatal framing result as described below. Status 8 preserves framing and request correlation and does not itself require connection closure.

## Limits And Validation

- Maximum key length: 250 bytes.
- Maximum value length: 1,048,576 bytes (1 MiB).
- Maximum request frame: 1,048,858 bytes.
- Maximum response frame: 1,048,596 bytes.
- Default maximum pending output per connection: 4 MiB.

The server validates magic, version, reserved bits, opcode, individual lengths, opcode-specific body rules, TTL range, and checked total frame size from the fixed header before waiting for a body. Input is retained only up to the bounded maximum request size plus one read chunk. If another encoded response would exceed the pending-output limit, the server closes that connection instead of allowing unbounded memory growth.

## Framing Errors

An incomplete header or body remains buffered until more TCP bytes arrive. Multiple complete requests in one read are decoded and answered in order, and an incomplete trailing request remains buffered.

Once a complete 32-byte header is available, invalid magic, version, reserved bits, opcode, lengths, body rules, or TTL range is an unrecoverable framing error. The server sends a `protocol_error` response using the header's request ID and then closes the connection after the response is flushed. If the peer closes while a frame is incomplete, the server simply closes the connection.

Validly framed cache errors such as `not_found`, `invalid_key`, or `item_too_large` receive their normal status response and do not close the connection.
