# HighCache

HighCache is a C++20/Linux high-performance multithreaded in-memory key-value
cache server. It implements a compact cache core, a Linux `epoll` TCP runtime,
and a versioned binary protocol as one focused systems-engineering project. It
is not a Redis clone, does not implement RESP, and is not a distributed cache.

## Architecture

```text
TCP client
  -> acceptor
  -> worker epoll event loop
  -> Connection buffers
  -> binary ProtocolCodec
  -> CacheEngine hash routing
  -> CacheShard mutex
  -> unordered_map + per-shard LRU + TimingWheel
  -> Slab or system value allocator
```

One acceptor thread owns the listen socket and the only TTL `timerfd`. It hands
accepted descriptors round-robin to worker threads through per-worker queues
and `eventfd`. Each worker exclusively owns its installed connections. All
workers share one `CacheEngine`; keys route to one shard, and each shard has one
exclusive mutex protecting its cache, LRU metadata, timer state, and value
allocator. GET takes that mutex too because a hit updates LRU recency.

See [the architecture guide](docs/architecture.md) for the exact GET/SET paths,
thread and lock ownership, cache-entry lifetime, TTL generation handling, and
shutdown sequence. [Design decisions](docs/design-decisions.md) explains why
the project uses sharding, `std::unordered_map`, per-shard LRU, a Timing Wheel,
Slab allocation, level-triggered `epoll`, and a custom binary protocol.

## Features

- binary-safe GET, SET, and DELETE over persistent TCP connections
- configurable logical memory limit with per-shard LRU eviction
- optional TTL rounded up to one-second ticks
- 60-slot single-level Timing Wheel with rounds for longer TTLs
- generation checks that make stale expiration events harmless
- hash-routed cache sharding and one mutex per shard
- selectable per-shard Slab or system value allocation
- Linux nonblocking sockets with level-triggered `epoll`
- one acceptor, configurable worker event loops, and clean signal shutdown
- fragmented-frame, pipelined-request, partial-write, and backpressure handling
- cache, allocator, and shutdown metrics
- typed error codes, level-filtered logging, and strict `key=value` config
- concurrent real-TCP benchmark client and retained raw Phase 7 results
- `-Wall -Wextra -Wpedantic`, GoogleTest, ASan, and UBSan integration

Keys are limited to 250 bytes and values to 1 MiB. Cache capacity charges
`key.size() + value.size()`; it deliberately excludes container metadata,
allocator rounding, reserved slabs, timer keys, and process RSS. Capacity and
LRU are shard-local, so one shard can evict while another still has space.

## Quick Start

Prerequisites are Linux, CMake 3.20 or newer, a C++20 compiler, and pthreads.
CMake uses an installed GoogleTest package when available or fetches the pinned
v1.15.2 release while configuring tests.

Build a normal Release tree:

```bash
./scripts/build.sh
```

Run the server in the foreground with the example configuration:

```bash
./scripts/run_server.sh
```

Then use the functional client from another terminal:

```bash
./build-release/highcache_client 127.0.0.1 11211 set greeting hello 5000
./build-release/highcache_client 127.0.0.1 11211 get greeting
./build-release/highcache_client 127.0.0.1 11211 delete greeting
```

The optional SET TTL is milliseconds. The server runs until `SIGINT` or
`SIGTERM`. Pass a different config path to `run_server.sh` or set the documented
environment variables shown by `./scripts/run_server.sh --help`.

Run a self-contained representative benchmark (one million measured requests,
80% GET / 20% SET, 128 loopback connections):

```bash
./scripts/run_benchmark.sh
```

The script starts and stops its own server on `127.0.0.1:11211`; that port must
be free. The full 54-million-request experiment matrix remains available as
`scripts/run_phase7_benchmarks.sh` and is not the default quick benchmark.

## Configuration

`config/highcache.conf.example` is the current server configuration. It covers
`log_level`, IPv4 `host`, `port`, `worker_threads`, logical
`cache_capacity_bytes`, `shard_count`, and the `allocator` backend (`slab` or
`system`). Blank lines and comments are ignored; unknown or duplicate keys,
malformed lines, unsupported values, and out-of-range numbers are reported as
typed configuration errors with a source line number.

## Tests and Sanitizers

Clean Debug build and test:

```bash
cmake -E remove_directory build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Clean AddressSanitizer and UndefinedBehaviorSanitizer build and test:

```bash
cmake -E remove_directory build-asan
cmake -S . -B build-asan \
  -DCMAKE_BUILD_TYPE=Debug \
  -DHIGHCACHE_ENABLE_SANITIZERS=ON
cmake --build build-asan -j
ctest --test-dir build-asan --output-on-failure
```

ThreadSanitizer is a separate build option, but it is not claimed as a final
validation result because the tested WSL2 runtime could not execute it
reliably.

## Measured Results

The committed measurements are loopback tests from one 4-core/8-thread Intel
i5-8300H WSL2 environment. They are not universal capacity claims.

- 64 shards delivered 163020.720 QPS versus 147405.814 QPS for one shard
  (+10.593%) in the representative mixed workload, but P95/P99 were worse and
  most throughput benefit had already appeared by four shards.
- At 256-byte values, the system allocator median was 162421.161 QPS and Slab
  was 161920.051 QPS (-0.309%). Slab reserved more memory and did not show a
  throughput advantage in that representative comparison.
- Profiling attributed about 25.45% of pre-change samples to zero-filling a
  64 KiB temporary receive buffer. Leaving it uninitialized and consuming only
  the bytes returned by `recv()` raised median throughput from 161681.761 to
  174477.848 QPS (+7.914%) and improved AVG/P50, while P95 regressed 28.363%
  and P99 regressed 16.774%.
- Client scaling peaked at four threads in this setup: approximately 58k, 95k,
  186k, and 163k QPS at 1, 2, 4, and 8 threads. Shared client/server resources
  and WSL scheduling are possible limitations, not proven causes.

The full methodology, raw-row selection rules, environment, profiler evidence,
and limitations are in [docs/benchmark.md](docs/benchmark.md). Wire constants
and framing behavior are in [docs/protocol.md](docs/protocol.md). The exact
stability, sanitizer, and final validation record is in
[docs/validation.md](docs/validation.md).

## Project Layout

```text
include/     public component interfaces
src/         cache, allocator, protocol, network, client, and server code
tests/       unit, concurrency, socket, and loopback integration tests
benchmark/   benchmark client, configuration, raw CSV, and perf text artifacts
docs/        architecture, decisions, protocol, benchmark, and validation docs
scripts/     normal workflows and the retained full benchmark matrix
config/      example server configuration
```

## Known Limits

HighCache is single-node and in-memory only: there is no replication,
persistence, distributed routing, or Redis compatibility. The server is
IPv4-only. TTL resolution is one second. Memory capacity and LRU are per-shard,
statistics are race-free moving aggregates rather than atomic global snapshots,
and the single-level Timing Wheel retains stale key-only events until their
scheduled slots. Benchmark results are constrained by WSL2, loopback traffic,
and client/server CPU sharing. See [design decisions](docs/design-decisions.md)
for the associated tradeoffs.
