# HighCache

## 项目简介

HighCache 是一个面向 Linux 的 C++20 多线程内存键值缓存服务器。项目从缓存内核、
内存管理、并发模型、网络事件循环到二进制协议形成了一条完整的数据通路，重点展示
系统编程中的所有权、并发控制、资源边界和基于测量的优化，而不是复刻 Redis 或实现
分布式缓存。

HighCache 不是 Redis Clone，也不是分布式缓存系统。项目关注 C++、并发、内存管理、
缓存算法、Linux 网络和性能工程。

## 核心特性

- 支持二进制安全的 GET、SET、DELETE 和持久 TCP 连接
- 以 `key.size() + value.size()` 计算逻辑容量，按分片执行 LRU 淘汰
- TTL 向上取整到一秒 tick，由 60 槽单层 Timing Wheel 管理
- 通过 key 与 generation 校验使覆盖、删除、淘汰后遗留的过期事件失效
- 按 key 哈希分片，每个 `CacheShard` 使用独立互斥锁
- 支持 Slab 与系统分配器两种 value 存储后端
- 使用非阻塞 socket、level-triggered `epoll`、`eventfd` 和 `timerfd`
- 一个 acceptor 线程配合可配置数量的 worker 事件循环
- 正确处理拆包、粘包、流水线请求、部分写与慢客户端背压
- 提供缓存、淘汰、TTL、分配器和连接关闭指标
- 提供类型化错误、分级日志和严格的 `key=value` 配置解析
- 集成 GoogleTest、`-Wall -Wextra -Wpedantic`、ASan 与 UBSan
- 附带真实 TCP 压测客户端、完整原始数据和 `perf` 分析记录

key 上限为 250 字节，value 上限为 1 MiB。容量统计有意不包含容器元数据、
分配器对齐、Slab 预留空间、定时器 key 和进程 RSS。容量与 LRU 都是分片局部的，
因此热点分片可能在其他分片仍有空余时发生淘汰。

## 系统架构

```text
TCP client
  -> acceptor thread
  -> worker epoll event loop
  -> Connection input/output buffers
  -> ProtocolCodec
  -> CacheEngine hash routing
  -> CacheShard mutex
  -> unordered_map + per-shard LRU + TimingWheel
  -> Slab or system value allocator
```

acceptor 线程独占监听 socket 和唯一的 TTL `timerfd`，通过每个 worker 的待接收队列
与 `eventfd` 轮询分发新连接。连接安装到 worker 后，仅由该 worker 访问。所有 worker
共享同一个 `CacheEngine`；每次 key 操作只路由到一个 shard，并持有该 shard 的互斥锁。
GET 同样需要独占锁，因为命中会更新 LRU 顺序与统计计数。

完整的 GET/SET 路径、线程与锁所有权、条目生命周期、TTL generation 规则和关闭顺序
见[架构说明](docs/architecture.md)。各项技术取舍见[设计决策](docs/design-decisions.md)。

## 核心设计

### Cache Sharding

`CacheEngine` 用 key 的哈希值选择一个 `CacheShard`。map、LRU、Timing Wheel 和 value
分配器均归 shard 所有，因此正常请求不会获取全局缓存锁，也不会同时持有多个 shard
锁。引擎级统计和 tick 按顺序访问各 shard，结果无数据竞争，但不是同一瞬间的全局快照。

### LRU 与容量淘汰

总容量按字节平均分配到各 shard，余数依次分给前几个 shard。SET 前先完成新 value 的
分配，再按需从本 shard 的 LRU 尾部淘汰；GET 与 SET 成功后都会把条目移到 MRU 端。
单条数据大于 shard 容量时直接返回 `item_too_large`。

### TTL 与 Timing Wheel

每个 shard 有一套 60 槽、一秒分辨率的 Timing Wheel。超过一圈的 TTL 通过 rounds
计数表示。缓存内核本身不创建线程，运行时由 acceptor 的 `timerfd` 唯一推进时间。

### Generation 与 stale timer

定时事件只持有复制后的 key、generation 和 rounds，不持有条目指针；事件到期时必须
同时匹配当前 key 与 generation，才能删除条目。generation 在 SET 时单调递增，不因
DELETE、LRU 淘汰或重新插入而重置，所以 stale timer 不能误删同 key 的新 value。

### Slab Allocator

Slab 后端提供 64 至 8192 字节的固定 size class，每个 class slab 为 1 MiB；更大的
value 回退到系统分配。每个 shard 独占自己的分配器，在已有 shard 锁下分配和释放，
无需额外的全局分配锁。该实现用于研究复用、预留和内部碎片，不预设性能结论。

### `epoll` 网络模型

acceptor 与每个 worker 都运行独立的 level-triggered `epoll` loop。acceptor 只负责
监听、连接交接、停止事件和 TTL tick；worker 独占已安装连接，并把读写操作排空到
`EAGAIN`。只有 output buffer 非空时才监听 `EPOLLOUT`。

### 自定义二进制协议

协议 v1 使用 32 字节请求头、20 字节响应头和大端整数，支持 request ID 关联及任意
二进制 key/value。每个连接默认最多保留 4 MiB 待发送数据；若下一响应会越界，服务器
关闭该连接。完整常量、字段与错误语义见[协议说明](docs/protocol.md)。

## 快速开始

### 环境要求

Linux、CMake 3.20 或更新版本、支持 C++20 的编译器和 pthread。配置测试时，
CMake 优先使用系统 GoogleTest，否则获取固定版本 v1.15.2。

### 编译

构建 Release：

```bash
./scripts/build.sh
ctest --test-dir build-release --output-on-failure
```

### 启动

以前台方式启动示例配置：

```bash
./scripts/run_server.sh
```

另开终端执行功能请求：

```bash
./build-release/highcache_client 127.0.0.1 11211 set greeting hello 5000
./build-release/highcache_client 127.0.0.1 11211 get greeting
./build-release/highcache_client 127.0.0.1 11211 delete greeting
```

SET 的可选 TTL 单位为毫秒。服务器持续运行至收到 `SIGINT` 或 `SIGTERM`。可向
`run_server.sh` 传入其他配置路径；完整参数见 `./scripts/run_server.sh --help`。

### Benchmark

运行一次可复现的代表性压测（100 万条计量请求、80% GET / 20% SET、128 个连接）：

```bash
./scripts/run_benchmark.sh
```

脚本会自行启动并停止 `127.0.0.1:11211` 上的服务器，因此该端口必须空闲。完整的
5400 万请求实验矩阵由 `scripts/run_benchmark_matrix.sh` 保留，不作为默认快速入口。

### 测试与 Sanitizer

Debug 构建和测试：

```bash
cmake -E remove_directory build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
ctest --test-dir build --output-on-failure
```

ASan、UBSan 和 LeakSanitizer 构建与测试：

```bash
cmake -E remove_directory build-asan
cmake -S . -B build-asan \
  -DCMAKE_BUILD_TYPE=Debug \
  -DHIGHCACHE_ENABLE_SANITIZERS=ON
cmake --build build-asan -j
ctest --test-dir build-asan --output-on-failure
```

项目还提供独立的 ThreadSanitizer 选项，但不把它列为最终验证结果：测试所用 WSL2
环境无法可靠执行 TSan 运行时。

## 配置

`config/highcache.conf.example` 包含当前服务器配置项：`log_level`、IPv4 `host`、
`port`、`worker_threads`、逻辑 `cache_capacity_bytes`、`shard_count`，以及
`allocator` 后端（`slab` 或 `system`）。解析器忽略空行和注释；未知项、重复项、
格式错误、不支持的值及越界数字都会产生带源文件行号的类型化配置错误。

## 性能测试

以下数据来自同一台 4 核 8 线程 Intel i5-8300H 的 WSL2 环境，均为本机 loopback
实测，不应解释为通用容量承诺。完整方法、逐行原始数据和限制见
[性能测试与分析](docs/benchmark.md)。

### Workload

256 字节 value、64 shards、Slab 后端；表中为三次运行按 QPS 取中位数对应的完整一行：

| 负载 | 读写比 | QPS | AVG us | P50 us | P95 us | P99 us |
|---|---|---:|---:|---:|---:|---:|
| A | 100% GET | 166843.287 | 749.551 | 594.500 | 1693.402 | 3525.802 |
| B | 80% GET / 20% SET | 161681.761 | 750.357 | 596.900 | 1666.600 | 3507.201 |
| C | 50% GET / 50% SET | 162728.766 | 772.364 | 595.400 | 1792.901 | 3931.701 |

### Sharding 实验

代表性混合负载中，1 shard 为 147405.814 QPS，64 shards 为 163020.720 QPS，
吞吐提升 10.593%。但 P95/P99 更差，主要吞吐收益在 4 shards 时已经出现；4 到 128
之间结果不单调，不能据此认为 shard 足多越好。

### Shard 数量实验

| Shards | 1 | 4 | 16 | 32 | 64 | 128 |
|---:|---:|---:|---:|---:|---:|---:|
| QPS | 147405.814 | 162872.309 | 157487.363 | 162719.380 | 163020.720 | 162906.096 |

### System Allocator vs Slab

在重复测试的 256 字节 value 点，系统分配器中位数为 162421.161 QPS，Slab 为
161920.051 QPS，即 -0.309%。Slab 预留更多内存，且这一负载没有证明其吞吐优势。

### Client Thread Scaling

服务器固定四个 worker、连接数固定 128：

| 客户端线程 | QPS |
|---:|---:|
| 1 | 58218.834 |
| 2 | 95208.316 |
| 4 | 186072.217 |
| 8 | 162858.403 |

该环境在四个客户端线程时达到峰值。客户端和服务器共享逻辑 CPU，WSL 调度也可能产生
影响，但实验没有证明八线程回落的唯一原因。

## `perf` 性能分析与优化

优化前约 25.45% 样本落在 libc `rep stos`：`Connection::handle_readable()` 每次
可读事件都会零填充一个 64 KiB 临时接收缓冲区。改为不初始化数组，并且只消费
`recv()` 返回的正长度前缀后，中位吞吐从 161681.761 提升到 174477.848 QPS，增幅
7.914%；AVG 与 P50 改善，但 P95 回退 28.363%，P99 回退 16.774%。该改动基于吞吐
和 CPU 收益保留，不宣称所有延迟分位均有改善。

## 稳定性验证

Release TCP 服务连续运行 1801 秒，294 次客户端循环共处理 294,000,000 条计量请求：

```text
successful requests: 294000000
failed requests:     0
failed invocations:  0
unexpected exit:     0
server exit status:  0
```

每轮客户端断开后，fd 起始值、最小值、最大值和最终值均为 21。RSS 从 94,368 KiB
开始，最大及最终值为 94,384 KiB。详细过程见[验证记录](docs/validation.md)。

## 项目结构

```text
include/     公开组件接口
src/         缓存、分配器、协议、网络、客户端和服务器实现
tests/       单元、并发、socket 与 loopback 集成测试
benchmark/   压测客户端、配置、原始 CSV 与 perf 文本产物
docs/        架构、设计决策、协议、性能和验证文档
scripts/     构建、运行、验证和完整压测脚本
config/      服务器示例配置
```

## 设计取舍

项目选择 shard-local map、LRU、Timing Wheel 与 allocator，使一次 key 操作只需一把
锁，并保持组件所有权清楚；相应代价是容量不能跨 shard 借用、全局 LRU 只是近似。
level-triggered `epoll` 让未排空工作可再次得到通知，代价是 readiness 触发更频繁。
Slab 被保留用于 allocator 设计与实验比较，而不是因为测试结果证明它更快。

## 已知限制

HighCache 只支持单节点内存存储，不提供复制、持久化、分布式路由或 Redis 兼容性；
服务器仅支持 IPv4，TTL 分辨率为一秒。容量与 LRU 按 shard 隔离；统计值是无数据竞争
的移动聚合，而非原子全局快照；单层 Timing Wheel 会保留只含 key 的过期旧事件，直到
其计划槽被处理。性能结果受 WSL2、loopback 以及客户端与服务器共享 CPU 的约束。

## 文档索引

- [架构说明](docs/architecture.md)：组件边界、线程模型、锁、数据路径和生命周期
- [设计决策](docs/design-decisions.md)：主要方案的理由、收益与代价
- [二进制协议](docs/protocol.md)：帧格式、状态码、限制与错误处理
- [性能测试与分析](docs/benchmark.md)：完整数据、方法、`perf` 证据和优化结果
- [验证记录](docs/validation.md)：构建、测试、Sanitizer 与长时间稳定性结果
