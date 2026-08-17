# HighCache

HighCache is a C++20/Linux project that will evolve into a high-performance multithreaded in-memory KV cache server.

## Current status

**Phase 0 project infrastructure is complete.**

The current implementation provides:

- C++20 CMake project
- `-Wall -Wextra -Wpedantic` on GCC and Clang
- optional AddressSanitizer and UndefinedBehaviorSanitizer instrumentation
- GoogleTest integration with a pinned fallback release
- typed errors with stable error codes
- thread-safe, level-filtered stream logging
- strict `key=value` configuration loading
- minimal `highcache_server` startup executable
- unit tests and a server smoke test

It intentionally does **not** implement a cache, commands, LRU, TTL, timing wheels, sharding, slab allocation, networking, a protocol, or benchmarks.

## Configuration

The server uses `info` logging by default. Pass an optional configuration file to change the level:

```bash
./build/highcache_server config/highcache.conf.example
```

Phase 0 configuration accepts one setting. Blank lines and lines beginning with `#` are ignored.

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
