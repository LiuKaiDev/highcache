# HighCache

HighCache is a C++20/Linux project that will evolve into a high-performance multithreaded in-memory KV cache server.

## Current status

**Phase 1 single-threaded cache core is complete.**

The current implementation provides:

- C++20 CMake project
- `-Wall -Wextra -Wpedantic` on GCC and Clang
- optional AddressSanitizer and UndefinedBehaviorSanitizer instrumentation
- GoogleTest integration with a pinned fallback release
- typed errors with stable error codes
- thread-safe, level-filtered stream logging
- strict `key=value` configuration loading
- single-threaded `std::unordered_map` cache storage
- SET, GET, and DELETE-equivalent operations
- explicit cache result statuses and key/value validation
- deterministic randomized cache correctness coverage
- minimal `highcache_server` startup executable
- unit tests and a server smoke test

It intentionally does **not** implement LRU or memory-capacity eviction, TTL, timing wheels, sharding or concurrency, slab allocation, networking, a protocol, or benchmarks.

## Cache core

`highcache::Cache` provides `set`, `get`, and `erase` operations with expected average O(1) behavior from `std::unordered_map`. Keys and values are copied into cache-owned `std::string` storage.

Cache operations return `CacheStatus` values instead of throwing for normal outcomes. Successful inserts and overwrites return `ok`; missing GET and DELETE operations return `not_found`. Invalid requests return `invalid_key`, `key_too_large`, or `value_too_large`. A failed GET leaves its output string unchanged.

Empty keys are invalid and empty values are supported. Keys may contain at most 250 bytes and values at most 1 MiB. These limits provide explicit, practical guardrails for the normal-allocation baseline and are not coupled to a network protocol.

The cache tests cover insertion, lookup, deletion, overwrites, ownership, boundary validation, independent state, and 100,000 fixed-seed randomized SET/GET/DELETE operations against a reference model. This is correctness testing, not a benchmark.

## Configuration

The server uses `info` logging by default. Pass an optional configuration file to change the level:

```bash
./build/highcache_server config/highcache.conf.example
```

Configuration accepts one setting. Blank lines and lines beginning with `#` are ignored.

```ini
log_level=debug
```

Supported levels are `debug`, `info`, `warning`, and `error`. Unknown keys, duplicate keys, malformed lines, and unsupported levels are reported as configuration errors.

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
ctest --test-dir build --output-on-failure
./build/highcache_server
```

## Sanitizer build

```bash
cmake -S . -B build-asan \
  -DCMAKE_BUILD_TYPE=Debug \
  -DHIGHCACHE_ENABLE_SANITIZERS=ON
cmake --build build-asan -j
ctest --test-dir build-asan --output-on-failure
./build-asan/highcache_server
```
