# HighCache Architecture

HighCache is a single-node, multithreaded in-memory cache server. The cache and
network layers are separate: protocol requests call the same `CacheEngine` API
that is covered directly by cache tests, while TCP integration tests exercise
the complete path.

## Component Map

```text
main thread
  Config -> Logger -> CacheEngine -> CacheServer

CacheServer acceptor thread
  listen socket + epoll + stop eventfd + TTL timerfd
      -> per-worker handoff queue + eventfd

Worker thread / event loop
  epoll -> Connection -> input/output Buffer -> ProtocolCodec
                                            -> shared CacheEngine

CacheEngine
  hash(key) -> CacheShard
                 mutex
                 ValueAllocator (Slab or system)
                 Cache
                   unordered_map<string, StoredEntry>
                   LRU list
                   TimingWheel
```

`CacheServer` owns the acceptor machinery and a configured set of workers.
Each `Worker` owns one `epoll` descriptor, one wakeup `eventfd`, its handoff
queue, and its installed `Connection` map. A `Connection` owns its socket and
input/output `Buffer` objects and calls `ProtocolCodec` for framing.

`CacheEngine` owns non-movable `CacheShard` objects. Each shard owns one mutex,
one selected `ValueAllocator`, and one `Cache`. The cache owns its key map, LRU
list, Timing Wheel, logical accounting, counters, and allocator-backed value
objects. `SlabAllocator` is one implementation of the allocator interface; the
system backend is the comparison implementation.

## Threads and Ownership

The process has these threads after startup:

- The main thread creates the engine and server, blocks `SIGINT`/`SIGTERM`, and
  waits synchronously with `sigwait`.
- One acceptor thread owns the listen socket event loop. It accepts all ready
  sockets, assigns them round-robin, processes the stop event, and reads the
  one-second `timerfd`.
- `worker_threads` worker threads each own an independent connection event
  loop. There is no thread per connection.

The acceptor creates a nonblocking, close-on-exec socket with `accept4`, moves
its `UniqueFd` into a worker's mutex-protected pending queue, and writes that
worker's `eventfd`. The worker drains the queue and registers the descriptor in
its own `epoll` set. From that point until close, only that worker accesses the
`Connection`; the acceptor never accesses it again. Workers share the cache
engine, not connections or per-worker cache copies.

Shutdown wakes and joins the acceptor first, then requests stop and joins every
worker. Remaining installed and pending `UniqueFd` objects close through RAII;
no thread is detached.

## Locks

There is one `std::mutex` per `CacheShard`. SET, GET, DELETE, tick processing,
and shard statistic reads hold that mutex exclusively. A normal key operation
hashes to one shard and never takes another shard lock or a global cache lock.

GET is not a read-only operation: a hit splices the entry to the front of the
LRU list and increments the hit counter. Therefore a shared lock would not make
GET shared without redesigning recency and statistics. Engine-wide metrics and
ticks visit and lock shards one at a time. They are race-free, but a metric sum
can observe different shards at slightly different moments.

The only other shared network structure is each worker's acceptor-to-worker
pending queue, protected by its own mutex. Installed connection maps are
worker-thread-exclusive.

## GET Request Flow

```text
TCP bytes
  -> worker receives EPOLLIN
  -> Connection::handle_readable calls recv
  -> received prefix appended to the input Buffer
  -> ProtocolCodec validates and decodes one complete frame
  -> CacheEngine hashes the key and selects one CacheShard
  -> shard mutex acquired
  -> Cache unordered_map lookup
  -> hit value copied and LRU node moved to MRU
  -> shard mutex released
  -> response encoded with the original request_id
  -> response appended to the output Buffer
  -> EPOLLOUT enabled while bytes remain
  -> worker sends until complete or EAGAIN
```

Incomplete request bytes and unsent response bytes remain buffered between
level-triggered events. Multiple complete requests in the input buffer are
processed in order.

## SET Request Flow

`ProtocolCodec` first validates header constants, opcode-specific body rules,
length bounds, and the TTL range. `CacheEngine` then hashes the validated key,
and the selected shard takes its mutex. `Cache::set` validates cache-level key,
value, and TTL rules and calculates the logical `key + value` charge.

An item larger than the shard capacity is rejected without eviction. For a
valid item, the replacement value is allocated before it replaces an existing
value. A monotonically increasing generation is assigned for every SET that
reaches mutation; a positive TTL schedules a key/generation event. LRU entries
are evicted from that shard until the new logical charge fits, the map/LRU state
is inserted or updated, and the old value is released through RAII. A SET hit
or insertion becomes most recently used.

Allocation or container exceptions propagate to the connection boundary and
produce `internal_error`. The allocated value remains RAII-safe. Capacity
evictions completed before a later map/list allocation failure are not rolled
back.

## Capacity and LRU

Total configured logical capacity is divided across shards. Each shard receives
`total / shard_count` bytes, with the first `total % shard_count` shards getting
one extra byte. Each shard maintains its own most-recently-used to
least-recently-used list. SET evicts from the selected shard's LRU tail until
the item fits. There is no global LRU or cross-shard borrowing, so aggregate
free capacity elsewhere cannot prevent a hot shard from evicting.

Logical usage counts key and value bytes only. Allocator metrics separately
report live value bytes, reserved backing, free block capacity, internal
fragmentation, and allocation counts.

## TTL Ownership and Stale Events

Each `Cache` has a 60-slot single-level Timing Wheel with one-second resolution.
A positive TTL rounds up to ticks; events beyond one revolution carry remaining
rounds. The cache core has no clock and creates no thread.

Exactly one runtime source advances time: the acceptor thread reads its
monotonic `timerfd` and calls `CacheEngine::tick()` once per elapsed interval.
Workers never tick the engine. An engine tick visits every shard once under its
shard lock.

Every shard-local cache has a monotonic 64-bit generation source. A timer event
owns a copied key, generation, and round count, never a `CacheEntry` pointer.
Overwrite, DELETE, or LRU eviction can leave an old event in the wheel. At its
due tick, a missing key or generation mismatch is ignored. Because generation
continues across delete and reinsert, an old event cannot delete a newer value
with the same key.

## Entry and Value Lifetime

The map owns each key string and `CacheEntry`. `CacheEntry` owns an allocator
pointer, value pointer, and exact requested size; it is movable but not
copyable. Its destructor returns the value through the same allocator and size
used for allocation. Values are length-delimited and may contain null bytes.

An entry leaves the cache through one of five paths:

- Explicit DELETE erases its LRU node and map element.
- Capacity eviction erases the shard-local LRU tail.
- TTL expiration erases a current key/generation match.
- Overwrite swaps in an already allocated replacement; the displaced RAII
  value releases as the temporary is destroyed.
- Cache destruction destroys all map entries before the owning allocator.

The common erase paths reduce logical usage before map destruction triggers
value release. Member ownership order ensures `Cache` is destroyed before a
`CacheShard` allocator, and a standalone cache's embedded allocator outlives
its entries.

## Protocol, Buffers, and Backpressure

Protocol version 1 has a 32-byte request header and 20-byte response header,
both encoded field by field in big-endian order. The protocol is request-ID
correlated and binary-safe. See [protocol.md](protocol.md) for exact fields,
statuses, bounds, and malformed-frame behavior.

Connection reads and writes continue through `EINTR` and stop at `EAGAIN`.
`EPOLLOUT` is registered only when output is pending. A connection may queue at
most 4 MiB of response data by default; exceeding the cap closes that
connection, preventing an unbounded slow-client queue. Invalid complete headers
receive one `protocol_error` response and close after the response is flushed.
