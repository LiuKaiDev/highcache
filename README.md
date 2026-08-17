# HighCache

HighCache is a C++20/Linux project that will evolve into a high-performance multithreaded in-memory KV cache server.

## Current status

**Bootstrap before Phase 0 completion.**

This starter intentionally contains only the project skeleton:

- C++20 CMake project
- warning configuration
- optional ASan/UBSan flags
- GoogleTest wiring
- minimal `highcache_server` executable
- bootstrap smoke test

It intentionally does **not** implement Cache, LRU, TTL, sharding, slab allocation, networking, protocol handling, or benchmarking.

Phase 0 should next implement the project's foundational `Logger`, `Config`, and error-handling model, together with tests.

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
ctest --test-dir build --output-on-failure
```

## Sanitizer build

```bash
cmake -S . -B build-asan \
  -DCMAKE_BUILD_TYPE=Debug \
  -DHIGHCACHE_ENABLE_SANITIZERS=ON
cmake --build build-asan -j
ctest --test-dir build-asan --output-on-failure
```
