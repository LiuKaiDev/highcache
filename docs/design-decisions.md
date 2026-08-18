# HighCache 设计决策

以下内容记录当前实现的关键选择及其代价，不表示这些方案对所有负载都最优。

## 使用 `std::unordered_map` 建立索引

标准哈希表提供平均 O(1) 查询、明确且成熟的所有权语义，也适合作为测量上层缓存行为
的可靠基线。自研开放寻址表可能改善局部性或减少分配，但会引入冲突处理、扩容、删除、
异常安全和额外基准验证。现有 profile 显示首要热点在网络 buffer 初始化，而非 map，
因此没有证据支持增加这部分复杂度。

## 每个 shard 独立维护 LRU

LRU 元数据与 map、锁共用同一个 shard 边界。命中或 SET 只需持有一个 mutex 即可更新
recency，淘汰也不需要全局协调锁。每个 shard 都有独立 LRU，不存在全局 LRU；容量不能
跨 shard 借用，淘汰顺序仅在 shard 内成立，因此繁忙 shard 可能在其他 shard 尚有空余
时淘汰数据。

## Shard 容量不做全局再平衡

总容量在 engine 创建时按字节分给各 shard，余数依次交给前几个 shard。运行时不跨
shard 借用容量，也不在淘汰时同时持有两把锁。这样维持单 shard 操作边界，避免迁移
条目、协调多把锁和维护全局 LRU 的成本；代价是 key 分布偏斜时，热点 shard 会在全局
仍有空余容量时淘汰数据。

## 使用 Timing Wheel 管理 TTL

60 槽、一秒分辨率的 Timing Wheel 避免周期扫描整个 map。调度和到期槽遍历的成本取决
于 timer 活跃度，而非缓存总条目数；rounds 可表达超过一圈的 TTL。当前实现接受一秒
粒度、重复 timer key，以及旧事件在计划槽到达前继续占用内存的代价。项目需求不需要
层级时间轮。

## 用 generation 保证 TTL 安全

timer 保存 key，不保存 entry 指针。每个进入修改流程的 SET 获得唯一、单调递增的
shard-local generation；到期删除要求 key 和 generation 同时匹配。generation source
不会因 DELETE、淘汰或重新插入而重置，否则旧 timer 可能再次取得相同 generation，
误删新 value。64 位 source 耗尽时抛出异常，不静默回绕。

## 分片而非全局锁

`CacheEngine` 按 key 哈希选择 shard，使不同 shard 中的 key 可在不同 mutex 下并发。
单 shard 正好构成全局锁基线。在实测混合负载中，64 shards 相比 1 shard 的 QPS 提升
10.593%，但尾延迟更差，而且主要吞吐收益到 4 shards 时已出现。默认 64 只是配置基线，
不是通用最优值；shard 数还会放大 Slab 最小预留，并把容量切得更细。

## 使用 `std::mutex` 而非 `std::shared_mutex`

GET 会复制 value、把 entry 移到 MRU，并更新计数，因此会修改 shard 状态，必须独占
同步。按当前数据路径，`shared_mutex` 无法使 GET 共享执行，其额外状态和调度成本也不
天然有利。要分离读写锁，需要采用不同的 recency 策略，并以受控测量提供依据。

## Slab 分配器

该分配器用于研究 size class、free-list 复用、物理预留、内部碎片和淘汰场景中的 value
生命周期。固定 class 从 64 到 8192 字节，每个 class slab 为 1 MiB；更大的 value
使用系统分配。

在重复测试的代表性 256 字节 value 负载中，系统分配器中位数为 162421.161 QPS，
Slab 为 161920.051 QPS，即 -0.309%，且 Slab 预留更多内存。这说明实现完成了分配器
工程实验，但该负载没有展示吞吐优势。其他 value size 只有单次探索结果，不提升为一般
性能结论。

## 每个 shard 独占分配器

每个 shard 拥有自己的 allocator，并保证 cache 先于 allocator 析构。分配和释放发生
在已经持有 shard mutex 的路径上，不需要全局 allocator lock 或第二把锁。指标跨 shard
聚合。代价是 free pool 彼此独立；使用 Slab 时，每个活跃 size class 在每个 shard 中
都可能至少预留 1 MiB。

## 使用 level-triggered `epoll`

level-triggered `epoll` 是清晰的非阻塞基线。handler 仍然把 accept、read、write 排空
到 `EAGAIN`，但如果工作没有完成，内核会再次报告 readiness，比 edge-triggered 更不易
因漏掉 rearm 状态而停滞。仅在 output queue 非空时启用 `EPOLLOUT`，避免持续 writable
通知。切换到 edge-triggered 或 one-shot 需要测量证据，也会增加状态复杂度。

## 自定义二进制协议

协议使用定宽、带版本 header，明确的大端序列化，按长度界定的二进制 key/value，以及
opaque `request_id`。固定 header 和长度边界让解析与 framing 保持简单且有界，二进制
安全 payload 可以原样承载任意字节，`request_id` 则支持流水线响应关联。

## 连接级背压

每个连接默认最多保留 4 MiB 待发送数据。若加入下一响应会越界，服务器关闭该连接。
这限制慢速或不读取客户端占用的内存，并让 worker 继续服务其他 descriptor。代价是硬
断开，而不是流量控制恢复或按客户端公平调度。

## 基于 profile 的优化

优化前约 25.45% 的采样落在 libc `rep stos`，调用链指向每次可读事件都零初始化
64 KiB 临时接收 buffer 的 `Connection::handle_readable()`。保留的改动让数组不初始化，
并且只追加 `recv()` 返回的正长度前缀；未写入字节不会被读取或复制，因此没有引入越界
或未初始化读取。

实测中位吞吐提升 7.914%，AVG/P50 改善，但在测试所用 WSL2 环境中 P95 回退
28.363%，P99 回退 16.774%。数据支持基于吞吐和 CPU 行为保留这一改动，不支持宣称
尾延迟普遍改善。
