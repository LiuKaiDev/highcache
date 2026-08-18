# HighCache 性能测试与分析

所有性能结果都来自真实 TCP 数据路径：

```text
highcache_benchmark -> TCP -> epoll server -> binary protocol -> CacheEngine
```

性能测试不直接调用 `CacheEngine`。所有重复测量均保存在
[`benchmark/results/phase7_results.csv`](../benchmark/results/phase7_results.csv)，没有删除
任何一次结果。

## 测试环境

数据采集于 2026-08-18：

| 项目 | 配置 |
|---|---|
| CPU | Intel Core i5-8300H @ 2.30 GHz |
| 拓扑 | 1 socket，4 个物理核心，2 threads/core，8 个逻辑 CPU |
| 内存 / swap | 7.7 GiB / 2.0 GiB |
| 环境 | WSL2，Microsoft hypervisor |
| Kernel | `6.18.33.2-microsoft-standard-WSL2` |
| 编译器 | GCC 13.3.0（`Ubuntu 13.3.0-6ubuntu2~24.04.1`） |
| CMake | 3.28.3 |
| perf | Ubuntu `linux-tools-6.8.0-134` 提供的 6.8.12 |
| Release flags | `-O3 -DNDEBUG -g -std=c++20 -Wall -Wextra -Wpedantic` |
| 服务器 | 4 workers，1 GiB 逻辑容量；除变量实验外使用 Slab |
| 客户端 | 8 threads，128 connections；除变量实验外 pipeline 1 |
| 数据集 | 100,000 个均匀分布 key，seed 12345 |
| 单次运行 | 1,000,000 条计量请求，10,000 条 warmup 请求 |

采用 1 GiB 逻辑容量是有意的控制变量：100,000 个 4096 字节 value 加 key 约需
411.7 MB。若使用 256 MiB，预加载数据会被淘汰，value size 对比就会混入命中率差异。

## 测量方法

客户端按固定 seed 预生成均匀 key 和精确 GET/SET 比例，每次调用只生成一份二进制
payload。计时前先连接所有客户端，以 SET 预加载全部 key，等待预加载完成，再执行不计量
的 warmup 并重置客户端统计。报告的请求数、duration、latency、throughput 和 hit ratio
均不包含 preload 与 warmup。

每个客户端线程拥有一个 `epoll` loop 和多个 nonblocking connection，不采用一连接
一线程，也没有全局请求 mutex。dispatch 使用 `std::chrono::steady_clock` 记录时间；
只有在响应完整解码并按 `request_id` 关联后，才结束 latency 观测。AVG 使用所有观测值，
P50、P95、P99 使用 nearest-rank 原始观测，不根据 AVG 估算。错误响应和传输失败保留
在总数中。

主要测量点重复三次。表格展示三次中 QPS 为中位数的那次完整结果，因此同一行的 latency
和 CPU 均来自同一次运行。64、1024、4096 字节的分配器结果明确是单次探索；256 字节
分配器对比重复三次。所有计量请求均成功，所有 GET 均命中预加载数据集。

服务器 CPU 从 `/proc/<pid>/stat` 读取，覆盖完整客户端调用（包含 preload 与 warmup），
再除以 wall time。四个 server worker 可并发运行，因此 CPU 可能超过 100%。RSS 在计量
结束后采样。RSS、逻辑缓存字节和 allocator reserved bytes 是三种不同指标。

## 基准负载

- A：100% GET
- B：80% GET / 20% SET
- C：50% GET / 50% SET

基准中位数使用 256 字节 value、64 shards 和 Slab：

| 负载 | QPS | AVG us | P50 us | P95 us | P99 us | Server CPU % | Hit ratio |
|---|---:|---:|---:|---:|---:|---:|---:|
| A | 166843.287 | 749.551 | 594.500 | 1693.402 | 3525.802 | 352.807 | 1.000 |
| B | 161681.761 | 750.357 | 596.900 | 1666.600 | 3507.201 | 346.477 | 1.000 |
| C | 162728.766 | 772.364 | 595.400 | 1792.901 | 3931.701 | 351.383 | 1.000 |

负载 B 的重复结果在 156332.875 至 163478.385 QPS 之间；负载 C 在 156262.104 至
165451.892 QPS 之间。这个波动体现 WSL/共享 CPU 噪声，也是没有选择单次最好成绩的原因。

## 全局锁基线与分片

单 shard 是自然的全局锁基线，因为所有 key 都获取同一个 `CacheShard` mutex。固定负载
B 及其他变量：

| Shards | QPS | AVG us | P50 us | P95 us | P99 us | Server CPU % |
|---:|---:|---:|---:|---:|---:|---:|
| 1 | 147405.814 | 850.884 | 739.100 | 1573.000 | 2533.200 | 346.100 |
| 64 | 163020.720 | 774.789 | 601.902 | 1799.990 | 3957.815 | 350.322 |

64 shards 的中位 QPS 提高 10.593%，AVG/P50 降低，但 P95/P99 明显升高。因此数据支持
这一测试中分片具有吞吐收益，不支持它改善所有延迟指标的说法。

## `malloc/free` 与 Slab

`allocator=system` 和 `allocator=slab` 在同一 cache、LRU、TTL、sharding、networking、
protocol 与逻辑容量规则下切换 `ValueAllocator` 后端。负载 C 提供足够 SET 以覆盖分配路径。

| 后端 | Bytes | Reps | QPS | AVG us | P50 us | P95 us | P99 us | RSS KiB | Reserved bytes |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| system | 64 | 1 | 158769.574 | 788.469 | 611.296 | 1828.707 | 3981.573 | 36264 | 6400000 |
| Slab | 64 | 1 | 161293.119 | 776.016 | 599.102 | 1814.606 | 4062.614 | 94204 | 67108864 |
| system | 256 | 3 | 162421.161 | 773.346 | 615.496 | 1747.006 | 3628.314 | 55064 | 25600000 |
| Slab | 256 | 3 | 161920.051 | 767.021 | 609.802 | 1751.693 | 3742.912 | 94252 | 67108864 |
| system | 1024 | 1 | 152645.822 | 815.754 | 646.802 | 1867.689 | 3788.479 | 130112 | 102400000 |
| Slab | 1024 | 1 | 153520.911 | 814.320 | 639.397 | 1865.594 | 4064.486 | 159976 | 134217728 |
| system | 4096 | 1 | 135364.884 | 918.250 | 744.996 | 2003.808 | 3943.679 | 447764 | 409600000 |
| Slab | 4096 | 1 | 142932.163 | 878.383 | 697.196 | 2013.989 | 3986.614 | 473120 | 452984832 |

在重复三次的 256 字节点，Slab 中位 QPS 低 0.309%，没有测得速度优势。Slab 还会预留
更多内存，因为 64 个 shard 对每个活跃 size class 都可能持有至少一个 1 MiB backing
slab。4096 字节点表面上的 Slab 增益只有一次探索结果，不作为性能结论。

Slab 是已经实现并验证的分配器工程实验，但这个负载没有证明它比系统分配器吞吐更高。

## Shard 数量

固定负载 B、256 字节 value 和 Slab：

| Shards | QPS | AVG us | P50 us | P95 us | P99 us | Server CPU % |
|---:|---:|---:|---:|---:|---:|---:|
| 1 | 147405.814 | 850.884 | 739.100 | 1573.000 | 2533.200 | 346.100 |
| 4 | 162872.309 | 765.632 | 633.000 | 1623.300 | 2906.600 | 351.395 |
| 16 | 157487.363 | 794.089 | 618.552 | 1790.760 | 3779.106 | 343.050 |
| 32 | 162719.380 | 767.191 | 605.858 | 1753.170 | 3560.938 | 349.128 |
| 64 | 163020.720 | 774.789 | 601.902 | 1799.990 | 3957.815 | 350.322 |
| 128 | 162906.096 | 771.985 | 602.097 | 1801.107 | 3930.115 | 350.314 |

主要 QPS 收益到 4 shards 时已经出现。4 至 128 的结果不单调，集中在约 157k 至 163k
QPS；32、64、128 的中位结果实际上都约为 163k。此机器上更多 shard 不保证更好，
同时还会增加 Slab 最小预留。

## 客户端线程扩展

只改变客户端线程数；服务器固定四个 worker，连接数固定 128：

| 客户端线程 | QPS | AVG us | P50 us | P95 us | P99 us | Server CPU % |
|---:|---:|---:|---:|---:|---:|---:|
| 1 | 58218.834 | 2196.563 | 1729.995 | 4133.015 | 4863.419 | 100.567 |
| 2 | 95208.316 | 1340.607 | 1248.604 | 1988.096 | 2539.994 | 196.522 |
| 4 | 186072.217 | 678.359 | 579.499 | 1262.803 | 2182.907 | 370.067 |
| 8 | 162858.403 | 770.267 | 607.296 | 1791.689 | 3651.177 | 347.681 |

这一环境在四个客户端线程时达到峰值。客户端与服务器共享八个逻辑 CPU，WSL 调度也可能
影响曲线，但实验没有证明其中任何一个是八线程回退的确定原因。为避免增加共享主机负载，
未测试 16 个客户端线程。

## `perf` 分析

系统安装的 `perf` 与 WSL kernel 不匹配，因此从 Ubuntu 包中无 root 解压 perf 6.8.12
和 `libtraceevent1`，并通过显式 `LD_LIBRARY_PATH` 运行。`-p` 可以计数，但采样需要
显式列出 `/proc/<pid>/task` 中逗号分隔的 thread ID。Release binary 保留 debug info，
没有对 sanitizer binary 做 profile。

以下是代表性命令，其中实际包路径缩写为 `perf`：

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

每条命令都与一次真实的 300 万或 500 万请求负载 B 客户端运行重叠。`perf stat` 正常完成：

| Counter | Value |
|---|---:|
| task-clock | 72391.26 ms，3.616 CPUs utilized |
| cycles | 59631311192 |
| instructions | 10267884685，0.17 IPC |
| branches | 2092920828 |
| branch misses | 102796594，4.91% |
| cache references | 5088907733 |
| cache misses | 230567168，4.53% |
| context switches / migrations | 0 / 0 |

WSL 环境把事件限制到 user space（`:u`），所以为零的 context-switch 和 migration 是
缺失的 kernel 数据，不表示调度从未发生。

`perf top` 每秒采集 107 至 118 个样本。最后的活跃快照中，未解析的 libc zero-fill
routine 占 25.79% 至 25.91%，`recv` 占 12.24% 至 13.89%，`epoll_ctl` 占
12.86% 至 15.21%，`send` 占 9.19% 至 9.28%，hash lookup 占 4.82% 至 6.65%。

优化前保存的 `perf record` 共捕获 1835 个样本，lost 为 0。主要条目为 libc routine
25.45%、`epoll_ctl` 14.01%、`recv` 13.19%、`send` 8.72%、cache hash lookup
6.54%。原始文本保存在 `benchmark/results/phase7_perf_stat.txt`、
`phase7_perf_top.txt`、`phase7_perf_record.txt` 和 `phase7_perf_report.txt`；临时二进制
`perf.data` 不提交到仓库。

## 瓶颈定位

反汇编把 libc 首要地址 `0x18954a` 解析为 `rep stos`，调用链落在
`Connection::handle_readable()`。该函数声明了 value-initialized
`std::array<char, 65536>`，每次可读事件都先清零全部 64 KiB，随后 `recv` 又覆盖实际
收到的前缀。这解释了优化前 25.45% 的样本，比任何有名称的 syscall 或 cache function
占比都高。

## 优化与复测

假设：不初始化栈上数组，并且只追加 `recv` 返回的正字节数，即可消除 64 KiB zero-fill，
同时不会观察未写入字节。实现只改变服务器中的数组声明；客户端代码与 workload 生成
没有变化。

完全相同的 100 万请求负载 B 在修改前后各运行三次，分别选择中位 QPS 对应的完整一行：

| 版本 | QPS | AVG us | P50 us | P95 us | P99 us | Server CPU s | Server CPU % |
|---|---:|---:|---:|---:|---:|---:|---:|
| 优化前 | 161681.761 | 750.357 | 596.900 | 1666.600 | 3507.201 | 24.23 | 346.477 |
| 优化后 | 174477.848 | 716.723 | 486.400 | 2139.299 | 4095.503 | 21.65 | 332.591 |

中位 QPS 提高 7.914%，AVG 降低 4.482%，P50 降低 18.512%，server CPU time 降低
10.648%。与此同时，P95 回退 28.363%，P99 回退 16.774%。保留该改动是因为测得吞吐
和 CPU 收益，并不宣称延迟全面改善。

优化后 profile 捕获 968 个样本，lost 为 0。zero-fill routine 消失；新的主要条目为
`epoll_ctl` 20.25%、`recv` 18.39%、`send` 10.02%、hash lookup 8.78%。确认结果
保存在 `benchmark/results/phase7_perf_report_after.txt`。

## 复现方式

使用已构建 Release binary 和固定混合负载参数完成一次实用的代表性测试：

```bash
./scripts/run_benchmark.sh
```

该快速入口独立于完整测量矩阵，不会向已提交的原始 CSV 追加数据。重建并运行完整矩阵：

```bash
cmake -E remove_directory build-release
cmake -S . -B build-release \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_FLAGS_RELEASE="-O3 -DNDEBUG -g"
cmake --build build-release -j
./scripts/run_phase7_benchmarks.sh baseline
HIGHCACHE_CODE_LABEL=after ./scripts/run_phase7_benchmarks.sh optimization
```

runner 每次重复都会重启服务器，并记录完整结果、server CPU/RSS、logical bytes 和
allocator metrics。脚本开头的环境变量可覆盖所有关键参数。

## 正确性

完成优化后，Debug 与 ASan/UBSan suite 都通过当时发现的 159 / 159 个测试。Release
编译在 `-Wall -Wextra -Wpedantic` 下没有 warning。仓库后续工程验证增至 160 个测试，
结果见[验证记录](validation.md)。

## 结果限制

- loopback 网络不代表物理网络。
- 客户端与服务器争用同一组 CPU 和内存带宽。
- WSL2 调度与宿主机活动带来可见噪声。
- `perf -p` 无法完成 hardware sampling，必须显式 thread ID 和 `cpu-clock:u` 软件事件。
- kernel-only perf counter 不可用。
- 结果只描述一台机器、一套工具链和 uniform-key workload。
- 非 256 字节的 allocator 测量点只有一次重复，仅供探索。
