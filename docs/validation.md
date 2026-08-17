# Final Validation Record

This record captures the Phase 8 validation performed on 2026-08-18 in the
working tree that started at commit `63cdec4`.

## Reproducible Commands

Normal Release build:

```bash
./scripts/build.sh
```

The verified clean run used the default `build-release` directory and produced
`highcache_server`, `highcache_client`, and `highcache_benchmark` without
compiler warnings. `./scripts/run_server.sh` was run under a controlled
`SIGTERM`; it logged the listen address, final metrics, and clean shutdown.

Representative benchmark:

```bash
./scripts/run_benchmark.sh
```

This starts a temporary server with `benchmark/phase7_server.conf`, runs one
million measured requests using 80% GET / 20% SET, 256-byte values, 100,000
keys, four client threads, 128 connections, seed 12345, and 10,000 warmup
requests, then stops the server. The observed run completed 1,000,000 of
1,000,000 requests successfully in 5.137 seconds at 194678.003 requests/sec.
This is a reproducibility smoke benchmark, not a replacement for the retained
Phase 7 experiment matrix.

Debug and CTest:

```bash
cmake -E remove_directory build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
ctest --test-dir build --output-on-failure
```

ASan, UBSan, and LeakSanitizer:

```bash
cmake -E remove_directory build-asan
cmake -S . -B build-asan \
  -DCMAKE_BUILD_TYPE=Debug \
  -DHIGHCACHE_ENABLE_SANITIZERS=ON
cmake --build build-asan -j
ctest --test-dir build-asan --output-on-failure
```

## Stability

The real Release TCP server ran continuously from
`2026-08-18T01:25:45+08:00` through `2026-08-18T01:55:46+08:00`, for an actual
elapsed duration of 1801 seconds. A priming client first populated the cache;
the measured loop then repeated the fixed mixed workload with four client
threads, 128 connections, 1,000,000 measured requests per iteration, 100,000
keys, 256-byte values, 80% GET / 20% SET, seed 12345, and 10,000 warmup
requests. Clients connected and disconnected for every iteration.

The run completed 294 iterations and processed 294,000,000 measured requests:

```text
successful requests: 294000000
failed requests:     0
failed invocations:  0
unexpected exit:     0
server exit status:  0
```

After each client cycle, the server had 21 open descriptors. The observed fd
range was 21-21 at those post-disconnect samples, and the final count was 21.
RSS was 94,368 KiB at the start, ranged from 94,368 to 94,384 KiB, and ended at
94,384 KiB. The server log reported 100,000 live entries and clean allocator
metrics at shutdown. No corrupted responses, transport failures, or temporary
client errors were recorded.

## Correctness Results

The Debug and ASan/UBSan CTest runs each discovered and passed 160 tests. The
sanitizer run emitted no ASan, UBSan, or LeakSanitizer diagnostics. Release and
Debug compilation completed without `-Wall -Wextra -Wpedantic` warnings.

ThreadSanitizer was not claimed: the known WSL2 runtime could not execute the
TSan build reliably. The stability and benchmark runs used the real IPv4
loopback TCP path, not direct cache calls.

## Scope and Limits

This record validates the existing Phase 0-7 implementation and Phase 8
documentation/reproducibility infrastructure. It does not claim distributed
behavior, physical-network performance, universal shard sizing, a Slab
throughput win, or a tail-latency improvement from the retained receive-buffer
optimization. See [benchmark.md](benchmark.md) and
[design-decisions.md](design-decisions.md) for measured tradeoffs.
