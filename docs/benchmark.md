# Benchmark and Profiling

Phase 7 uses real measurements from the TCP path:

```text
highcache_benchmark -> TCP -> epoll server -> binary protocol -> CacheEngine
```

Direct `CacheEngine` calls are not used for performance results. Raw repeated
measurements are in
[`benchmark/results/phase7_results.csv`](../benchmark/results/phase7_results.csv).
No repetitions were removed.

## Environment

Measurements were collected on 2026-08-18 in this environment:

| Item | Value |
|---|---|
| CPU | Intel Core i5-8300H @ 2.30 GHz |
| Topology | 1 socket, 4 physical cores, 2 threads/core, 8 logical CPUs |
| RAM / swap | 7.7 GiB / 2.0 GiB |
| Environment | WSL2, Microsoft hypervisor |
| Kernel | `6.18.33.2-microsoft-standard-WSL2` |
| Compiler | GCC 13.3.0 (`Ubuntu 13.3.0-6ubuntu2~24.04.1`) |
| CMake | 3.28.3 |
| perf | 6.8.12 from Ubuntu `linux-tools-6.8.0-134` |
| Release flags | `-O3 -DNDEBUG -g -std=c++20 -Wall -Wextra -Wpedantic` |
| Server | 4 workers, 1 GiB logical capacity, Slab unless varied |
| Client | 8 threads, 128 connections, pipeline 1 unless varied |
| Dataset | 100,000 uniform keys, seed 12345 |
| Run | 1,000,000 measured requests and 10,000 warmup requests |

The 1 GiB logical capacity is deliberate: 100,000 values of 4096 bytes plus
keys require about 411.7 MB. A 256 MiB cache would evict preload data and turn
the value-size comparison into a miss-rate comparison.

## Methodology

The client precomputes uniformly selected keys and an exact GET/SET mix from a
fixed seed. It generates one binary payload per invocation. Before measurement
it connects all clients, preloads every key with SET, completes the preload,
runs the unmeasured warmup, and resets client statistics. The one-million
request count, duration, latency, throughput, and hit ratio exclude preload and
warmup.

Each client thread owns one epoll loop and multiple nonblocking connections;
there is no thread per connection or global client request mutex. Dispatch is
timestamped with `std::chrono::steady_clock`. A latency observation ends only
after the response is fully decoded and correlated by `request_id`. AVG uses
all observed durations. P50, P95, and P99 use nearest-rank observations, not an
estimate from AVG. Failed responses and transport failures remain in totals.

Primary points have three repetitions. Tables show the complete run whose QPS
is the median of the three, so every latency and CPU value on a row comes from
the same run. The 64, 1024, and 4096-byte allocator points are explicitly
single-run exploratory checks; the 256-byte allocator comparison is repeated
three times. Every measured request succeeded and every GET hit the preloaded
dataset.

Server CPU is `/proc/<pid>/stat` process CPU over the complete client
invocation, including preload and warmup, divided by wall time. It may exceed
100% because four server workers run concurrently. RSS is sampled after the
measured run. RSS, logical cache bytes, and allocator-reserved bytes are
different quantities.

## Workloads

- A: 100% GET
- B: 80% GET / 20% SET
- C: 50% GET / 50% SET

Baseline medians use 256-byte values, 64 shards, and Slab allocation:

| Workload | QPS | AVG us | P50 us | P95 us | P99 us | Server CPU % | Hit ratio |
|---|---:|---:|---:|---:|---:|---:|---:|
| A | 166843.287 | 749.551 | 594.500 | 1693.402 | 3525.802 | 352.807 | 1.000 |
| B | 161681.761 | 750.357 | 596.900 | 1666.600 | 3507.201 | 346.477 | 1.000 |
| C | 162728.766 | 772.364 | 595.400 | 1792.901 | 3931.701 | 351.383 | 1.000 |

Workload B repetitions ranged from 156332.875 to 163478.385 QPS; Workload C
ranged from 156262.104 to 165451.892 QPS. This visible WSL/shared-CPU noise is
why a single best run is not used.

## Global Lock vs Sharding

One shard is the natural global-lock baseline because every key takes the same
`CacheShard` mutex. With Workload B and all other controls fixed:

| Shards | QPS | AVG us | P50 us | P95 us | P99 us | Server CPU % |
|---:|---:|---:|---:|---:|---:|---:|
| 1 | 147405.814 | 850.884 | 739.100 | 1573.000 | 2533.200 | 346.100 |
| 64 | 163020.720 | 774.789 | 601.902 | 1799.990 | 3957.815 | 350.322 |

The 64-shard median has 10.593% higher QPS and lower AVG/P50, but materially
higher P95/P99. The data supports a throughput benefit for sharding in this
test, not a claim that it improves every latency measure.

## malloc/free vs Slab

`allocator=system` and `allocator=slab` select `ValueAllocator` backends under
the same cache, LRU, TTL, sharding, networking, protocol, and logical capacity
behavior. Workload C supplies enough SET traffic to exercise allocation.

| Backend | Bytes | Reps | QPS | AVG us | P50 us | P95 us | P99 us | RSS KiB | Reserved bytes |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| system | 64 | 1 | 158769.574 | 788.469 | 611.296 | 1828.707 | 3981.573 | 36264 | 6400000 |
| Slab | 64 | 1 | 161293.119 | 776.016 | 599.102 | 1814.606 | 4062.614 | 94204 | 67108864 |
| system | 256 | 3 | 162421.161 | 773.346 | 615.496 | 1747.006 | 3628.314 | 55064 | 25600000 |
| Slab | 256 | 3 | 161920.051 | 767.021 | 609.802 | 1751.693 | 3742.912 | 94252 | 67108864 |
| system | 1024 | 1 | 152645.822 | 815.754 | 646.802 | 1867.689 | 3788.479 | 130112 | 102400000 |
| Slab | 1024 | 1 | 153520.911 | 814.320 | 639.397 | 1865.594 | 4064.486 | 159976 | 134217728 |
| system | 4096 | 1 | 135364.884 | 918.250 | 744.996 | 2003.808 | 3943.679 | 447764 | 409600000 |
| Slab | 4096 | 1 | 142932.163 | 878.383 | 697.196 | 2013.989 | 3986.614 | 473120 | 452984832 |

At the repeated 256-byte point, Slab is 0.309% lower in median QPS. That is no
measured speed advantage. Slab also reserves more memory because each of 64
shards owns at least one 1 MiB backing slab. The apparent 4096-byte Slab gain is
only one exploratory repetition and is not treated as a performance claim.

The Slab allocator is an implemented allocator-engineering experiment, but this
workload did not demonstrate a throughput advantage over the system allocator.

## Shard Count

Workload B, 256-byte values, and Slab allocation were held fixed:

| Shards | QPS | AVG us | P50 us | P95 us | P99 us | Server CPU % |
|---:|---:|---:|---:|---:|---:|---:|
| 1 | 147405.814 | 850.884 | 739.100 | 1573.000 | 2533.200 | 346.100 |
| 4 | 162872.309 | 765.632 | 633.000 | 1623.300 | 2906.600 | 351.395 |
| 16 | 157487.363 | 794.089 | 618.552 | 1790.760 | 3779.106 | 343.050 |
| 32 | 162719.380 | 767.191 | 605.858 | 1753.170 | 3560.938 | 349.128 |
| 64 | 163020.720 | 774.789 | 601.902 | 1799.990 | 3957.815 | 350.322 |
| 128 | 162906.096 | 771.985 | 602.097 | 1801.107 | 3930.115 | 350.314 |

The main QPS benefit appears by four shards. Results from 4 through 128 are
non-monotonic and cluster around 157k-163k QPS; 32, 64, and 128 are effectively
flat at about 163k in their median runs. More shards are not always better on
this machine, and they increase minimum Slab reservation.

## Thread Scaling

Only client threads change; the server remains at four workers and connections
remain at 128:

| Client threads | QPS | AVG us | P50 us | P95 us | P99 us | Server CPU % |
|---:|---:|---:|---:|---:|---:|---:|
| 1 | 58218.834 | 2196.563 | 1729.995 | 4133.015 | 4863.419 | 100.567 |
| 2 | 95208.316 | 1340.607 | 1248.604 | 1988.096 | 2539.994 | 196.522 |
| 4 | 186072.217 | 678.359 | 579.499 | 1262.803 | 2182.907 | 370.067 |
| 8 | 162858.403 | 770.267 | 607.296 | 1791.689 | 3651.177 | 347.681 |

Scaling peaks at four client threads in this environment. The client and server
share eight logical CPUs, and WSL scheduling may also affect the curve, but the
experiment did not establish either as the cause of the eight-thread
regression. Sixteen client threads were omitted to avoid additional shared-host
load.

## perf Findings

The installed `perf` command did not match the WSL kernel. Ubuntu's perf 6.8.12
package and `libtraceevent1` were extracted without root and run with an
explicit `LD_LIBRARY_PATH`. Counting works through `-p`; sampling required the
explicit comma-separated `/proc/<pid>/task` thread IDs. Release binaries retain
debug information. Sanitizer binaries were not profiled.

Representative commands, with the actual package path abbreviated as `perf`:

```bash
./build-release/highcache_server benchmark/phase7_server.conf &
server_pid=$!
server_tids=$(ls "/proc/${server_pid}/task" | paste -sd, -)

perf stat --timeout 20000 \
  -e task-clock,cycles,instructions,branches,branch-misses,cache-references,cache-misses,context-switches,cpu-migrations \
  -p "${server_pid}" -o benchmark/results/phase7_perf_stat.txt

timeout -s INT 15 perf top --stdio --delay 5 --entries 30 \
  -F 99 -e cpu-clock:u -t "${server_tids}"

timeout -s INT 15 perf record -F 99 -e cpu-clock:u -g \
  --call-graph dwarf,8192 -t "${server_tids}" \
  -o /tmp/highcache-phase7-perf.data
perf report --stdio --no-children --percent-limit 0.5 \
  --sort comm,dso,symbol -i /tmp/highcache-phase7-perf.data
```

Each command overlapped a real 3- or 5-million-request Workload B client run.
`perf stat` completed normally and reported:

| Counter | Value |
|---|---:|
| task-clock | 72391.26 ms, 3.616 CPUs utilized |
| cycles | 59631311192 |
| instructions | 10267884685, 0.17 IPC |
| branches | 2092920828 |
| branch misses | 102796594, 4.91% |
| cache references | 5088907733 |
| cache misses | 230567168, 4.53% |
| context switches / migrations | 0 / 0 |

The events were restricted to user space (`:u`) by this WSL environment, so
zero context-switch and migration counts are unavailable kernel data, not a
claim that scheduling never occurred.

`perf top` collected 107-118 samples/s. Its last active snapshots put the
unresolved libc zero-fill routine at 25.79-25.91%, `recv` at 12.24-13.89%,
`epoll_ctl` at 12.86-15.21%, `send` at 9.19-9.28%, and hash lookup at
4.82-6.65%.

The saved pre-change `perf record` captured 1835 samples with zero lost. Its
leading entries were the libc routine at 25.45%, `epoll_ctl` at 14.01%, `recv`
at 13.19%, `send` at 8.72%, and cache hash lookup at 6.54%. Raw text is retained
in `benchmark/results/phase7_perf_stat.txt`, `phase7_perf_top.txt`,
`phase7_perf_record.txt`, and `phase7_perf_report.txt`; the temporary binary
`perf.data` is not committed.

## Bottleneck

Disassembly resolves the leading libc address `0x18954a` to `rep stos`. Its
call chain is `Connection::handle_readable()`. That function declared a
value-initialized `std::array<char, 65536>`, clearing all 64 KiB on every
readable event before `recv` overwrote the received prefix. This explained
25.45% of the pre-change samples and was larger than any named syscall or cache
function.

## Optimization

Hypothesis: leave the stack buffer uninitialized and append only the positive
byte count returned by `recv`. No unwritten byte is observed, while the 64 KiB
zero-fill is eliminated. Only the server declaration changed; client code and
benchmark generation did not.

The identical one-million-request Workload B comparison selected the
median-QPS run from each set of three:

| Version | QPS | AVG us | P50 us | P95 us | P99 us | Server CPU s | Server CPU % |
|---|---:|---:|---:|---:|---:|---:|---:|
| Before | 161681.761 | 750.357 | 596.900 | 1666.600 | 3507.201 | 24.23 | 346.477 |
| After | 174477.848 | 716.723 | 486.400 | 2139.299 | 4095.503 | 21.65 | 332.591 |

Median QPS improved 7.914%, AVG fell 4.482%, P50 fell 18.512%, and server CPU
time fell 10.648%. P95 regressed 28.363% and P99 regressed 16.774%; the change
is retained for its measured throughput/CPU benefit, not claimed as a uniform
latency improvement.

The post-change profile captured 968 samples with zero lost. The zero-fill
routine disappeared; the new leaders were `epoll_ctl` at 20.25%, `recv` at
18.39%, `send` at 10.02%, and hash lookup at 8.78%. The saved confirmation is
`benchmark/results/phase7_perf_report_after.txt`.

## Reproduction

For a practical one-command representative run using the retained Release
binaries and fixed mixed-workload defaults:

```bash
./scripts/run_benchmark.sh
```

That quick entry point is separate from the full measurement matrix below and
does not append to the committed Phase 7 CSV.

```bash
cmake -E remove_directory build-release
cmake -S . -B build-release \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_FLAGS_RELEASE="-O3 -DNDEBUG -g"
cmake --build build-release -j
./scripts/run_phase7_benchmarks.sh baseline
HIGHCACHE_CODE_LABEL=after ./scripts/run_phase7_benchmarks.sh optimization
```

The runner restarts the server for each repetition and records every result,
server CPU/RSS, logical bytes, and allocator metrics. Environment variables at
the top of the script provide explicit overrides.

## Correctness

After the optimization, both the Debug suite and the ASan/UBSan suite passed
159 of 159 discovered tests. Release compilation emitted no warnings under
`-Wall -Wextra -Wpedantic`.

## Limitations

- Loopback networking does not represent a physical network.
- Client and server contend for the same CPUs and memory bandwidth.
- WSL2 scheduling and host activity introduce visible noise.
- Hardware sampling did not work through `perf -p`; explicit thread IDs and
  the `cpu-clock:u` software event were required.
- Kernel-only perf counters were unavailable.
- Results describe one machine, toolchain, and uniform-key workload.
- Non-256-byte allocator points have one repetition and are exploratory.
