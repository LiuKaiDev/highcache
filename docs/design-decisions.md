# HighCache Design Decisions

These decisions describe the implemented baseline and its tradeoffs. They are
not claims that each choice is optimal for every workload.

## `std::unordered_map` as the Index

The cache uses a standard hash map because it supplies average constant-time
lookup, stable well-understood ownership, and a reliable baseline for measuring
higher-level cache behavior. A custom open-addressing table could improve
locality or reduce allocations, but it would add collision, growth, deletion,
exception-safety, and benchmarking work that the retained profile did not
justify. Profiling found network buffer initialization, not the map design, as
the leading hotspot.

## Per-Shard LRU

LRU metadata follows the same shard boundary as the map and lock. A hit or SET
can update recency while holding exactly one mutex, and eviction never needs a
global coordination lock. The tradeoff is approximate global behavior: a busy
shard can evict while another shard has unused capacity, and there is no single
process-wide least-recently-used ordering.

## Timing Wheel

A 60-slot, one-second Timing Wheel avoids periodically scanning every map entry
to discover expirations. Scheduling and each due-slot traversal depend on timer
activity rather than total cache size, while round counters support TTLs beyond
one revolution. The baseline accepts one-second granularity, duplicated timer
keys, and stale events that remain allocated until their scheduled slot. A
hierarchical wheel was not needed for the project scope.

## Generations for TTL Safety

Timers store keys, not entry pointers. Each successful SET receives a unique,
monotonically increasing shard-local generation, and expiration requires both
key and generation to match. The source is not reset by DELETE, eviction, or
reinsert; otherwise an old timer could acquire the same generation and delete a
new value. Exhausting the 64-bit source throws instead of silently wrapping.

## Sharding Instead of One Global Lock

`CacheEngine` hashes each key to one shard, allowing unrelated keys in different
shards to proceed under different mutexes. A one-shard engine is the global-lock
baseline. In the measured mixed workload, 64 shards produced 10.593% more QPS
than one shard, but tail latency was worse and most throughput benefit appeared
by four shards. The default of 64 is a configuration baseline, not a universal
optimum; shard count also multiplies minimum Slab reservation and partitions
capacity more finely.

## `std::mutex` Rather Than `std::shared_mutex`

GET copies a value, moves the entry to most-recently-used, and updates counters.
It therefore mutates shard state and requires exclusive synchronization. A
`shared_mutex` would not make the current GET path shared, and its extra state
and scheduling costs are not automatically beneficial. Read/write separation
would require a different recency policy and evidence from a controlled
workload.

## Slab Allocator

The allocator was implemented to study size classes, free-list reuse, physical
reservation, fragmentation, and value lifetime under cache eviction. It uses
fixed classes from 64 through 8192 bytes, 1 MiB class slabs, and system
allocation for larger values.

Slab did not outperform the system allocator in the representative 256-byte
Phase 7 workload. System allocation measured 162421.161 median QPS versus
161920.051 for Slab (-0.309%), and Slab reserved more memory.

The Slab allocator is an implemented allocator-engineering experiment, but this
workload did not demonstrate a throughput advantage over the system allocator.

The exploratory single-run value-size points are not promoted to a general
performance claim.

## Per-Shard Allocator Ownership

Every shard owns its allocator and destroys its cache before that allocator.
Allocation and deallocation happen while the shard mutex is already held, so no
global allocator lock or second lock is necessary. Metrics aggregate across
shards. The tradeoff is independent free pools and, with Slab, potentially one
minimum 1 MiB reservation per active size class per shard.

## Level-Triggered `epoll`

Level-triggered `epoll` was selected as a clear nonblocking baseline. Handlers
still drain accept, read, and write operations until `EAGAIN`, but readiness is
reported again if work remains, which makes incomplete progress less fragile
than an edge-triggered implementation. `EPOLLOUT` is enabled only while output
is queued to avoid continuous writable notifications. Edge-triggered or
one-shot modes would require evidence and more rearm-state complexity.

## Custom Binary Protocol, Not RESP

The protocol uses fixed-width versioned headers, explicit big-endian encoding,
length-delimited binary keys/values, and opaque request IDs. This keeps parsing
bounded and makes pipelined response correlation explicit. Redis RESP and Redis
command compatibility were intentionally not implemented because HighCache is
not a Redis clone; compatibility semantics would be a separate product goal,
not an internal framing substitution.

## Connection Backpressure

Each connection caps pending output at 4 MiB by default. If another response
would exceed the cap, the server closes that connection. This bounds memory
retained by a slow or non-reading client and keeps the worker responsive to
other descriptors. The tradeoff is a hard disconnect rather than flow-control
recovery or per-client fairness scheduling.

## Profiling-Driven Optimization

The pre-change profile attributed about 25.45% of captured samples to libc
`rep stos`, traced to zero-initializing a 64 KiB temporary receive buffer on
every readable event. The retained change leaves that array uninitialized and
appends only the positive byte count returned by `recv()`. No unwritten byte is
read or copied, so the change is memory-safe.

The measured median throughput gain was 7.914%, and AVG/P50 improved, but P95
regressed 28.363% and P99 regressed 16.774% in the tested WSL2 environment.
That supports retaining the zero-fill removal for throughput and CPU behavior;
it does not support a universal tail-latency improvement claim.
