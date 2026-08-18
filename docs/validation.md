# HighCache 最终验证记录

本文记录 2026-08-18 在初始提交 `63cdec4` 所对应实现上完成的工程验证。记录覆盖构建、
测试、Sanitizer、真实 TCP 快速压测与长时间稳定性，不把无法可靠运行的项目写成通过。

## 可复现命令

### Release 构建

```bash
./scripts/build.sh
```

经验证的 clean build 使用默认 `build-release` 目录，生成 `highcache_server`、
`highcache_client` 和 `highcache_benchmark`，没有编译器 warning。随后在受控
`SIGTERM` 下运行 `./scripts/run_server.sh`，日志包含监听地址、最终指标和正常关闭过程。

### 代表性快速压测

```bash
./scripts/run_benchmark.sh
```

脚本使用 `benchmark/benchmark_server.conf` 启动临时服务器，执行 100 万条计量请求：
80% GET / 20% SET、256 字节 value、100,000 个 key、4 个客户端线程、128 个连接、
seed 12345，以及 10,000 条 warmup 请求，随后停止服务器。该次验证完成
1,000,000 / 1,000,000 条成功请求，用时 5.137 秒，吞吐 194678.003 requests/sec。
它是复现路径的 smoke benchmark，不替代保留的完整实验矩阵。

### Debug 与 CTest

```bash
cmake -E remove_directory build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
ctest --test-dir build --output-on-failure
```

### ASan、UBSan 与 LeakSanitizer

```bash
cmake -E remove_directory build-asan
cmake -S . -B build-asan \
  -DCMAKE_BUILD_TYPE=Debug \
  -DHIGHCACHE_ENABLE_SANITIZERS=ON
cmake --build build-asan -j
ctest --test-dir build-asan --output-on-failure
```

## 长时间稳定性

真实 Release TCP 服务从 `2026-08-18T01:25:45+08:00` 连续运行至
`2026-08-18T01:55:46+08:00`，实际经过 1801 秒。priming client 先填充缓存；计量
循环随后反复执行固定混合负载：4 个客户端线程、128 个连接、每轮 1,000,000 条计量
请求、100,000 个 key、256 字节 value、80% GET / 20% SET、seed 12345 和 10,000 条
warmup 请求。客户端每轮都会连接并断开。

运行完成 294 轮，共处理 294,000,000 条计量请求：

```text
successful requests: 294000000
failed requests:     0
failed invocations:  0
unexpected exit:     0
server exit status:  0
```

每轮客户端断开后的服务器 fd 都是 21：起始值、观测最小值、最大值和最终值均为 21。
RSS 起始为 94,368 KiB，观测范围为 94,368 至 94,384 KiB，最终为 94,384 KiB。
关闭时日志报告 100,000 个 live entry 和正常的 allocator 指标。过程中未记录响应损坏、
传输失败或临时客户端错误。

## 正确性结果

Debug 与 ASan/UBSan CTest 均发现并通过 160 个测试。Sanitizer 运行未输出 ASan、UBSan
或 LeakSanitizer diagnostic。Release 与 Debug 编译在 `-Wall -Wextra -Wpedantic`
下均无 warning。

ThreadSanitizer 不列为通过项：已知 WSL2 运行时无法可靠执行该 TSan build。稳定性和
压测均使用真实 IPv4 loopback TCP 数据路径，而非直接调用 cache API。

## 结论边界

这些结果验证当前单节点实现及文档化的复现流程，不宣称分布式行为、物理网络性能、
通用 shard 数量、Slab 吞吐优势，也不宣称保留的接收 buffer 优化改善尾延迟。具体实测
取舍见[性能测试与分析](benchmark.md)和[设计决策](design-decisions.md)。
