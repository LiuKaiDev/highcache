# Phase 7 Benchmark and Profiling

This document records the reproducible Phase 7 network benchmark, controlled
experiments, profiling evidence, and the resulting focused optimization. All
published measurements use the real TCP path through the epoll server and
binary protocol. Direct `CacheEngine` calls are not used for performance
results.

## Environment

Final environment metadata and exact Release flags are recorded with the
completed measurements below.

## Methodology

`highcache_benchmark` precomputes deterministic uniformly distributed keys and
an exact GET/SET mix from seed 12345. It connects all clients, preloads the
entire key space with binary-safe SET values, completes an unmeasured warmup,
then resets statistics before the measured plan. Preload and warmup requests
are excluded from request count, duration, throughput, latency, and hit ratio.

Each client thread owns one epoll loop and multiple nonblocking TCP
connections. There is no thread per connection and no global client request
mutex. Request dispatch is timestamped with `std::chrono::steady_clock`; the
matching fully decoded response completes the observation. `request_id`
correlates multiple outstanding operations. AVG uses all observations and P50,
P95, and P99 use nearest-rank percentiles from the observed latency samples.
Failed responses and transport failures remain in the request totals.

Primary points use 1,000,000 measured requests, 100,000 preloaded keys, 10,000
warmup requests, 128 connections, pipeline depth 1, four fixed server workers,
and three repetitions. Tables report every repetition in
`benchmark/results/phase7_results.csv`; headline comparisons use medians.
Client and server share the same machine. Server CPU is process CPU consumed
over the complete client invocation, including preload and warmup, while QPS
and latency cover measured requests only. RSS is sampled after the measured
run and remains distinct from logical cache bytes and allocator-reserved bytes.

## Workloads

- A: 100% GET
- B: 80% GET / 20% SET
- C: 50% GET / 50% SET

Values are generated once per invocation. The size investigation uses exact
binary payloads of 64, 256, 1024, and 4096 bytes.

## Reproduction

```bash
cmake -E remove_directory build-release
cmake -S . -B build-release \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_FLAGS_RELEASE="-O3 -DNDEBUG -g"
cmake --build build-release -j
./scripts/run_phase7_benchmarks.sh baseline
```

The script changes one target variable at a time, restarts the server for every
repetition, and records client output, server CPU/RSS, logical cache usage, and
allocator metrics. Environment variables at the top of the script permit
explicit overrides. `HIGHCACHE_CODE_LABEL=after` with the `optimization` mode
appends the post-change repetitions without deleting baseline rows.

## Global Lock vs Sharding

The one-shard configuration is the natural global-lock baseline because all
keys share one `CacheShard` mutex. The controlled curve is 1, 4, 16, 32, 64,
and 128 shards with Workload B and 256-byte values.

## malloc/free vs Slab

`allocator=system` and `allocator=slab` select two `ValueAllocator` backends
under the same cache, LRU, TTL, sharding, network, protocol, and logical
capacity behavior. The representative 256-byte Workload C point has three
repetitions per backend; the other required value sizes are exploratory points.

## Shard Count

Final repeated results and the observed flattening point are recorded after the
baseline run.

## Thread Scaling

The client curve uses 1, 2, 4, and 8 threads while holding 128 connections,
server workers, workload, value size, shards, and request count fixed. The
machine has eight logical CPUs, so 16 client threads are omitted rather than
oversubscribing it further while client and server already share those CPUs.

## perf Findings

`perf stat`, non-interactive `perf top`, and `perf record`/`perf report` output
will be retained as text under `benchmark/results/`. Optimized Release binaries
retain debug information; sanitizer binaries are not profiled.

## Bottleneck

No bottleneck is selected before baseline profiling.

## Optimization

The retained change, hypothesis, identical before/after repetitions, medians,
and measured difference are recorded only after profiling and validation.

## Limitations

- Loopback networking does not represent a physical network.
- Client and server contend for the same CPU and memory resources.
- WSL2 scheduling and host activity introduce noise.
- Results describe one machine, toolchain, and uniform-key workload.
