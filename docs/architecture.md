# HighCache 架构说明

HighCache 是单节点、多线程内存缓存服务器。缓存层与网络层相互独立：协议请求调用的
`CacheEngine` API 由缓存测试直接覆盖，TCP 集成测试则验证从 socket 到缓存内核的
完整路径。

## 组件关系

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

`CacheServer` 管理 acceptor 及一组 worker。每个 `Worker` 独占一个 `epoll` 描述符、
一个唤醒用 `eventfd`、一个连接交接队列和已安装的 `Connection` map。`Connection`
持有 socket 和输入/输出 `Buffer`，并调用 `ProtocolCodec` 完成帧解析与编码。

`CacheEngine` 持有不可移动的 `CacheShard` 对象。每个 shard 独占一个互斥锁、一个选定
的 `ValueAllocator` 和一个 `Cache`；后者管理 key map、LRU 链表、Timing Wheel、
逻辑容量、计数器与分配器支持的 value。`SlabAllocator` 和系统后端均实现同一分配接口。

## 线程与所有权

服务启动后包含以下线程：

- main thread 创建 engine 和 server，屏蔽 `SIGINT`/`SIGTERM`，再通过 `sigwait`
  同步等待信号。
- 一个 acceptor thread 独占监听 socket 的事件循环，接受所有就绪连接、轮询分发、
  处理停止事件，并读取每秒触发的 `timerfd`。
- `worker_threads` 个 worker thread 各自运行独立连接事件循环；不采用一连接一线程。

acceptor 通过 `accept4` 创建 nonblocking、close-on-exec socket，把其 `UniqueFd` 移入
某个 worker 由互斥锁保护的 pending queue，再写该 worker 的 `eventfd`。worker 排空
队列并把描述符注册到自己的 `epoll`。自此直至关闭，只有该 worker 访问对应
`Connection`；acceptor 不再接触它。worker 共享 cache engine，不共享连接，也不持有
各自独立的缓存副本。

关闭时先唤醒并 join acceptor，再请求所有 worker 停止并逐一 join。尚未处理或已安装
的 `UniqueFd` 均由 RAII 关闭，没有 detached thread。

## 锁模型

每个 `CacheShard` 只有一个 `std::mutex`。SET、GET、DELETE、tick 处理和 shard 统计
读取都持有该锁。常规 key 操作哈希到一个 shard 后，不会再获取其他 shard 锁或全局
缓存锁。

GET 不是只读操作：命中会把条目 splice 到 LRU 头部并增加 hit 计数。因此在不重新设计
recency 与统计机制的前提下，shared lock 不能让 GET 共享执行。引擎级指标和 tick 每次
只锁一个 shard；访问没有数据竞争，但汇总值可能对应各 shard 的不同时刻。

网络侧唯一的共享结构是每个 worker 的 acceptor-to-worker pending queue，由各自的
mutex 保护。已安装连接的 map 只在所属 worker thread 内访问。

## GET 数据路径

```text
TCP bytes
  -> worker receives EPOLLIN
  -> Connection::handle_readable calls recv
  -> received prefix appended to input Buffer
  -> ProtocolCodec validates and decodes one complete frame
  -> CacheEngine hashes key and selects one CacheShard
  -> shard mutex acquired
  -> Cache unordered_map lookup
  -> hit value copied and LRU node moved to MRU
  -> shard mutex released
  -> response encoded with original request_id
  -> response appended to output Buffer
  -> EPOLLOUT enabled while bytes remain
  -> worker sends until complete or EAGAIN
```

不完整请求与未发送完的响应会保留在 buffer 中，等待后续 level-triggered 事件。输入
buffer 内的多个完整请求按顺序处理。

## SET 数据路径

`ProtocolCodec` 先校验 header 常量、opcode 对应的 body 规则、长度上限及 TTL 范围。
`CacheEngine` 对通过校验的 key 取哈希，选中的 shard 再加锁。`Cache::set` 检查缓存层
的 key、value 和 TTL 规则，并计算 `key + value` 逻辑占用。

若 item 大于 shard 容量，不淘汰任何条目并直接拒绝。合法 item 会先完成替换 value
的分配，再覆盖旧 value。每个进入修改流程的 SET 都获得递增 generation；正 TTL 会
调度 key/generation 事件。随后从该 shard 的 LRU 尾部淘汰，直至新条目可容纳，再插入
或更新 map/LRU，旧 value 通过 RAII 释放。插入或覆盖后的条目成为 MRU。

分配或容器异常会传播到连接边界并返回 `internal_error`，已分配 value 仍由 RAII 保证
释放。如果先完成了容量淘汰，之后 map/list 分配失败，已经发生的淘汰不会回滚。

## 容量与 LRU

总逻辑容量分配给各 shard：每个 shard 获得 `total / shard_count` 字节，前
`total % shard_count` 个 shard 各多一个字节。每个 shard 独立维护 MRU 到 LRU 的顺序，
SET 从所选 shard 的尾部淘汰，直至 item 可放入。系统没有全局 LRU 或跨 shard 借用，
因此其他 shard 的空闲容量不能阻止热点 shard 淘汰。

逻辑使用量只统计 key 与 value 字节。分配器指标另行报告 live value bytes、reserved
backing、free block capacity、internal fragmentation 与 allocation counts。

## TTL 所有权与过期旧事件

每个 `Cache` 拥有一个 60 槽、一秒分辨率的单层 Timing Wheel。正 TTL 向上取整为
tick，超过一圈的事件携带剩余 rounds。缓存内核不读取时钟，也不创建线程。

运行时只有一个时间源：acceptor thread 读取 monotonic `timerfd`，每个已流逝 interval
调用一次 `CacheEngine::tick()`。worker 不推进时间。一次 engine tick 逐个访问 shard，
并在处理时持有对应 shard 锁。

每个 shard-local cache 维护单调递增的 64 位 generation source。timer event 持有复制
后的 key、generation 和 rounds，绝不持有 `CacheEntry` 指针。覆盖、DELETE 或 LRU
淘汰可能在 wheel 中留下旧事件；到期时，如果 key 不存在或 generation 不匹配，事件被
忽略。generation 不因删除或重新插入而重置，所以旧事件不能删除同 key 的新 value。

## 条目与 value 生命周期

map 拥有 key string 和 `CacheEntry`。`CacheEntry` 保存 allocator 指针、value 指针和
请求的精确大小；它可移动但不可复制。析构时使用原 allocator 和 size 归还 value。
value 按长度界定，可以包含 null byte。

条目通过五条路径离开缓存：

- 显式 DELETE 删除其 LRU node 与 map element。
- 容量淘汰删除 shard-local LRU tail。
- TTL 到期删除 key/generation 均匹配的当前条目。
- 覆盖先换入已分配的新 value，被替换的 RAII value 随临时对象销毁而释放。
- Cache 析构先销毁所有 map entry，再销毁所属 allocator。

公共删除路径先减少逻辑使用量，再由 map 析构触发 value 释放。成员声明顺序保证
`Cache` 先于 `CacheShard` allocator 析构；独立 cache 内嵌的 allocator 同样晚于 entry。

## 协议、缓冲区与背压

协议 v1 的请求头为 32 字节，响应头为 20 字节；所有多字节整数逐字段按大端编码。
协议支持二进制数据并通过 request ID 关联响应。精确字段、状态码、边界与错误帧行为
见[二进制协议](protocol.md)。

连接读写在 `EINTR` 后继续，在 `EAGAIN` 时停止。仅当输出未清空时注册 `EPOLLOUT`。
每个连接默认最多排队 4 MiB 响应数据，超过上限就关闭该连接，避免慢客户端无限占用
内存。收到完整但非法的 header 时，服务器发送一次 `protocol_error`，刷新响应后关闭。
