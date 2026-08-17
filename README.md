# HighCache

HighCache is a C++20/Linux project that will evolve into a high-performance multithreaded in-memory KV cache server.

## Current status

**Phase 6 - epoll Network Server is complete.**

The current implementation provides:

- C++20 CMake project
- `-Wall -Wextra -Wpedantic` on GCC and Clang
- optional AddressSanitizer and UndefinedBehaviorSanitizer instrumentation
- optional standalone ThreadSanitizer instrumentation
- GoogleTest integration with a pinned fallback release
- typed errors with stable error codes
- thread-safe, level-filtered stream logging
- strict `key=value` configuration loading
- single-threaded shard-local `std::unordered_map` cache storage
- thread-safe hash-routed `CacheEngine` with 64 shards by default
- SET, GET, and DELETE-equivalent operations
- explicit cache result statuses and key/value validation
- configurable logical memory capacity and automatic LRU eviction
- optional per-SET TTL with deterministic one-second logical ticks
- a 60-slot single-level Timing Wheel with rounds for long TTLs
- generation-based stale timer protection
- cache-local hit, miss, eviction, and expiration counters
- per-shard slab allocation for cached value bytes
- allocator reservation, usage, reuse, and fragmentation metrics
- Linux nonblocking TCP serving with one acceptor and configurable workers
- level-triggered `epoll` event loops with `eventfd` connection handoff
- a versioned, big-endian binary protocol for GET, SET, and DELETE
- partial read/write, fragmented frame, pipelining, and output backpressure handling
- server-owned TTL advancement through one monotonic `timerfd`
- deterministic randomized cache correctness coverage
- real `highcache_server` and a small `highcache_client` command-line client
- unit, socket, and loopback TCP integration tests

It intentionally does **not** implement Phase 7 benchmark or performance work.

## Cache core

`highcache::Cache` provides `set`, `get`, and `erase` operations backed by `std::unordered_map`. Keys remain cache-owned `std::string` objects. Values are copied byte-for-byte into allocator-backed RAII storage and are not assumed to be null-terminated, so embedded null bytes are preserved. Recency is tracked from most to least recently used; successful insertions, overwrites, and GET hits make an entry most recently used. GET is non-const because a hit mutates this recency metadata.

Cache operations return `CacheStatus` values instead of throwing for normal outcomes. Successful inserts and overwrites return `ok`; missing GET and DELETE operations return `not_found`. Invalid requests return `invalid_key`, `key_too_large`, `value_too_large`, or `invalid_ttl`. An otherwise valid item larger than the cache capacity returns `item_too_large` without evicting or changing existing state. A failed GET leaves its output string unchanged.

Empty keys are invalid and empty values are supported. Keys may contain at most 250 bytes and values at most 1 MiB. These limits provide explicit, practical guardrails and are not coupled to a network protocol.

Capacity is selected when constructing a cache and defaults to 64 MiB. Memory usage is a deterministic logical charge of `key.size() + value.size()` per entry; container nodes, allocator metadata, string capacity, and process memory are intentionally excluded. SET evicts least-recently-used entries until a valid item fits. `capacity_bytes()` and `memory_usage_bytes()` expose the configured limit and current charge.

Successful GET hits increment `hit_count()`, valid missing GETs increment `miss_count()`, and each capacity-driven eviction increments `eviction_count()`. Genuine TTL expirations increment `expired_count()` and do not count as capacity evictions. Stale timers and explicit DELETE operations do not increment either expiration or eviction counters.

## Slab allocator

`highcache::SlabAllocator` has fixed 64, 128, 256, 512, 1024, 2048, 4096, and 8192-byte size classes. Each class owns one or more 1 MiB aligned slabs and selects the smallest class that can contain a request. A free block stores the next pointer in its own storage, giving O(1) free-list allocation and deallocation without a separate heap node per block. Freed blocks remain reserved by their class and are reused until the allocator is destroyed.

Slab backing regions use aligned `operator new` and the matching aligned `operator delete`. Their base alignment is `alignof(std::max_align_t)`, and every class stride is a multiple of that alignment. Requests above 8192 bytes use ordinary `operator new` and the matching `operator delete`, which provide alignment for ordinary C++ object storage. `allocate(0)` returns `nullptr`; zero-size allocation and null deallocation do not change metrics. For every non-null allocation, callers must pass the exact original requested size to `deallocate`.

Allocator metrics have these definitions:

- `allocated_bytes`: 1 MiB for every reserved slab plus the exact requested bytes of live large-object allocations
- `used_bytes`: requested bytes of all currently live allocations
- `free_bytes`: reusable size-class block capacity in currently reserved slabs
- `allocation_count`: successful non-zero allocation calls
- `deallocation_count`: valid non-null deallocation calls
- `internal_fragmentation`: for live slab allocations, the sum of `size_class - requested_size`
- `slab_count`: currently reserved backing slabs across all classes

Cache values own their allocator pointer, data pointer, and logical size through a non-copyable RAII object. Overwrite allocates the replacement before swapping ownership; the displaced value is then returned to the allocator. DELETE, LRU eviction, TTL expiration, and cache destruction remove the owning entry and return its value storage through the same path. A standalone `Cache` owns an embedded allocator. A `CacheShard` owns its allocator before its cache, so all entries are destroyed before allocator teardown.

Cache logical capacity and allocator physical metrics are separate. Capacity eviction still uses exactly `key.size() + value.size()` per entry and does not charge size-class rounding, slab reservations, container nodes, or allocator metadata. Allocator metrics describe value-buffer backing and can be read locally or aggregated by `CacheEngine`.

Genuine system allocation failure propagates as `std::bad_alloc`; normal cache outcomes continue to use `CacheStatus`. Value ownership remains safe during exceptions. The insertion path does not provide strong rollback if list or map allocation fails after entries have already been evicted to make capacity, so those completed evictions may remain.

## TTL and logical time

`set` accepts an optional TTL without changing persistent calls:

```cpp
cache.set("persistent", "value");
cache.set("temporary", "value", std::chrono::milliseconds{1500});
```

A zero TTL creates a persistent entry. A positive TTL expires after the requested duration rounded upward to the next one-second tick, so a positive sub-second TTL lasts until the first tick. A negative TTL returns `invalid_ttl` without changing the entry, its LRU position, accounting, generation, or existing expiration.

The cache core itself has no clock or background thread. Each explicit `cache.tick()` call advances logical cache time by exactly one second and processes the next slot of a 60-slot single-level Timing Wheel. When the Phase 6 TCP server is running, one `timerfd` in the acceptor event loop is the sole tick owner and calls `CacheEngine::tick()` once for every elapsed one-second interval. Workers never advance cache time. Long TTLs use rotation rounds, with a timer placed at `(current_slot + ticks) % 60` and carrying `(ticks - 1) / 60` remaining rounds.

Each successful SET assigns a cache-wide monotonically increasing generation. A timer owns only its key, scheduled generation, and remaining rounds; it never references a cache entry. Missing keys and generation mismatches make old timers harmless after overwrite, DELETE, or LRU eviction and reinsertion.

A current timer removes the entry from storage and the LRU list, releases its logical memory charge, and increments `expired_count()`. Timing Wheel nodes and duplicated timer keys are excluded from `memory_usage_bytes()`, which remains the deterministic `key.size() + value.size()` cache-entry charge rather than process RSS or total metadata memory.

## Sharding and thread safety

`highcache::Cache` and `SlabAllocator` remain unsynchronized, single-threaded components. Each `CacheShard` owns one `std::mutex`, one allocator, and one cache; SET, GET, DELETE, tick, and local statistic reads all hold that shard's exclusive lock. The same lock therefore protects its value allocator without a second allocator mutex or global allocator lock. GET cannot use a shared/read lock because a hit updates LRU recency.

`highcache::CacheEngine` owns non-movable shards through `std::unique_ptr`, defaults to 64 shards, and accepts total logical capacity and shard count at construction. A zero shard count is rejected with `invalid_argument`. Each key is routed to exactly one shard with `std::hash<std::string_view>{}(key) % shard_count`, so ordinary key operations never take a global lock or more than one shard lock.

Total capacity is divided with `base = total / shard_count`; the first `total % shard_count` shards receive one additional byte. Capacity and LRU are shard-local after partitioning. One shard may therefore evict entries while another still has unused capacity; Phase 4 does not rebalance memory or provide a global LRU.

Engine cache and allocator statistics visit shards sequentially and lock one shard at a time. They are race-free but represent a moving aggregate under concurrent mutation, not an atomic snapshot across all shards. `CacheEngine::tick()` similarly advances each shard exactly once under its own lock. There is still no background timer thread.

The tests cover insertion, lookup, deletion, overwrites, ownership, binary values, size-class boundaries, alignment, multi-slab growth, deterministic block reuse, exact allocator metrics, large-object fallback, LRU and TTL release paths, stale timers, shard routing, shared-key contention, and concurrent metrics. Network coverage includes fragmented and pipelined frames, malformed input, forced partial writes, peer disconnects, shared-cache visibility across workers, TTL expiry, clean shutdown, and 128 simultaneous loopback clients. Fixed-seed correctness scenarios include single-threaded reference models, a 100,000-operation concurrent mixed cache workload, and a bounded-live-set 1,000,000-operation allocator stress test. These are correctness tests, not benchmarks, and no performance claim is made.

## Network server

The Linux server has one acceptor thread and a configurable number of worker event loops. The acceptor drains `accept4()` until `EAGAIN`, assigns each new nonblocking socket round-robin, places its move-only descriptor in the selected worker's mutex-protected queue, and wakes that worker through `eventfd`. The worker registers and exclusively owns the connection from then on.

Each worker has its own level-triggered `epoll` descriptor, wakeup descriptor, connection map, and thread. All workers call the same thread-safe, sharded `CacheEngine`; there is no network-wide cache lock and no per-worker cache copy. `EPOLLOUT` is enabled only while a connection has queued output. A stop event wakes the acceptor, which is joined before the workers are stopped and joined, so no thread is detached.

Connections retain incomplete input and unsent output between events. Reads and writes continue through partial progress and `EINTR`, then stop at `EAGAIN`. Complete pipelined requests are handled in order. Pending output is limited to 4 MiB per connection; a response that would exceed the limit closes that connection. File descriptors use move-only RAII ownership and close-on-exec.

The binary protocol uses fixed-width, explicitly encoded big-endian headers and preserves request IDs across pipelined operations. Values are length-delimited and remain binary-safe through the protocol, cache, and Slab-backed storage. See [docs/protocol.md](docs/protocol.md) for the complete wire format and error policy.

## Configuration

The server listens on `127.0.0.1:11211` with four workers and `info` logging by default. Pass an optional configuration file to override those settings:

```bash
./build/highcache_server config/highcache.conf.example
```

Blank lines and lines beginning with `#` are ignored.

```ini
log_level=debug
host=127.0.0.1
port=11211
worker_threads=4
```

Supported levels are `debug`, `info`, `warning`, and `error`. `host` must be a nonempty IPv4 address, `port` must be from 0 through 65535, and `worker_threads` must be from 1 through 1024. Port 0 requests an OS-assigned ephemeral port, which is useful for tests. Unknown keys, duplicate keys, malformed lines, unsupported levels, and invalid numeric values are reported as configuration errors.

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
ctest --test-dir build --output-on-failure
./build/highcache_server config/highcache.conf.example
```

The server logs its actual listen address and port, then runs until `SIGINT` or `SIGTERM`. In another terminal, use the included client for basic operations:

```bash
./build/highcache_client 127.0.0.1 11211 set greeting hello 5000
./build/highcache_client 127.0.0.1 11211 get greeting
./build/highcache_client 127.0.0.1 11211 delete greeting
```

The client prints the wire status followed by a response body when present. It is a functional verification tool, not a load generator or benchmark.

## Sanitizer build

```bash
cmake -S . -B build-asan \
  -DCMAKE_BUILD_TYPE=Debug \
  -DHIGHCACHE_ENABLE_SANITIZERS=ON
cmake --build build-asan -j
ctest --test-dir build-asan --output-on-failure
```

AddressSanitizer/UndefinedBehaviorSanitizer and ThreadSanitizer are intentionally separate modes. To build with ThreadSanitizer:

```bash
cmake -S . -B build-tsan \
  -DCMAKE_BUILD_TYPE=Debug \
  -DHIGHCACHE_ENABLE_TSAN=ON
cmake --build build-tsan -j
ctest --test-dir build-tsan --output-on-failure
```
