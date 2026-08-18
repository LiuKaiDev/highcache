# HighCache 二进制协议

HighCache 协议 v1 是运行在持久 TCP 字节流上的请求/响应协议。所有整数使用无符号定宽
字段和大端字节序。header 逐字段序列化，不直接传输 C++ 对象内存。key 与 value 均按
长度界定，可以包含 null byte。

## 请求帧

请求 header 固定为 32 字节，随后依次为 `key_length` 字节 key 和 `value_length`
字节 value。

| 偏移 | 字段 | 大小 | 字节序 | 含义 |
|---:|---|---:|---|---|
| 0 | `magic` | 4 | 大端 | `0x48434143`（`HCAC`） |
| 4 | `version` | 1 | 不适用 | `1` |
| 5 | `opcode` | 1 | 不适用 | `1` GET，`2` SET，`3` DELETE |
| 6 | `reserved` | 2 | 大端 | 必须为 0 |
| 8 | `request_id` | 8 | 大端 | 客户端选择的不透明关联 ID |
| 16 | `key_length` | 4 | 大端 | key body 字节数 |
| 20 | `value_length` | 4 | 大端 | value body 字节数 |
| 24 | `ttl_ms` | 8 | 大端 | SET 生命周期，单位毫秒 |
| 32 | key | `key_length` | 不适用 | 原始 key 字节 |
| 可变 | value | `value_length` | 不适用 | 原始 value 字节 |

GET 与 DELETE 要求 `value_length == 0` 且 `ttl_ms == 0`。SET 携带 key 与 value；
`ttl_ms == 0` 表示不过期，正数传入缓存 TTL API，并向上取整到下一个一秒 cache tick。
大于 `INT64_MAX` 的 TTL 会被判为协议错误。

`request_id` 不表示服务器端顺序。服务器把它原样复制到对应响应中，因此客户端可以在
同一连接上发送多个流水线请求，而不必假设最多只有一个未完成请求。

## 响应帧

响应 header 固定为 20 字节，后随 `value_length` 字节 value。

| 偏移 | 字段 | 大小 | 字节序 | 含义 |
|---:|---|---:|---|---|
| 0 | `magic` | 4 | 大端 | `0x48434143`（`HCAC`） |
| 4 | `version` | 1 | 不适用 | `1` |
| 5 | `status` | 1 | 不适用 | 下表定义的稳定状态码 |
| 6 | `reserved` | 2 | 大端 | 0 |
| 8 | `request_id` | 8 | 大端 | 从请求复制 |
| 16 | `value_length` | 4 | 大端 | 响应 body 字节数 |
| 20 | value | `value_length` | 不适用 | GET value 或协议错误详情 |

GET 成功时返回缓存字节；SET 与 DELETE 成功时通常为空 body。GET 或 DELETE 查无 key
时返回空 body 的 `not_found`。

## Opcode

| 值 | 名称 | Body | TTL |
|---:|---|---|---|
| 1 | GET | 仅 key | 必须为 0 |
| 2 | SET | key 后接 value | 0 或正毫秒数 |
| 3 | DELETE | 仅 key | 必须为 0 |

协议 v1 不定义其他操作。

## 状态码

| 值 | 名称 | 含义 |
|---:|---|---|
| 0 | `ok` | 操作成功完成 |
| 1 | `not_found` | key 不存在 |
| 2 | `invalid_key` | key 为空 |
| 3 | `key_too_large` | key 超出缓存上限 |
| 4 | `value_too_large` | value 超出缓存上限 |
| 5 | `item_too_large` | item 无法放入缓存容量 |
| 6 | `invalid_ttl` | TTL 不被缓存接受 |
| 7 | `protocol_error` | 帧违反 wire format |
| 8 | `internal_error` | 执行请求时发生非预期异常 |

状态 0 至 6 属于正常缓存结果，响应后连接继续可用。状态 7 是下文所述的致命 framing
错误。状态 8 仍保持帧完整和 request ID 关联，本身不要求关闭连接。

## 限制与校验

- key 最大长度：250 字节。
- value 最大长度：1,048,576 字节（1 MiB）。
- 请求帧最大长度：1,048,858 字节。
- 响应帧最大长度：1,048,596 字节。
- 每个连接默认最大待发送数据：4 MiB。

服务器在等待 body 前，根据固定 header 校验 magic、version、reserved bits、opcode、
各字段长度、opcode 特定 body 规则、TTL 范围及经过溢出检查的帧总长度。输入最多保留
有界的最大请求长度加一个读取 chunk。如果再编码一个响应会超过 pending-output 上限，
服务器关闭该连接，避免内存无界增长。

## 拆包、粘包与错误处理

header 或 body 不完整时，现有字节保留在 buffer 中等待后续 TCP 数据。同一次 read 中
的多个完整请求按顺序解码并响应，末尾不完整请求继续保留。

一旦取得完整 32 字节 header，非法 magic、version、reserved bits、opcode、length、
body 规则或 TTL 范围都属于不可恢复的 framing 错误。服务器使用 header 中的 request ID
发送一个 `protocol_error` 响应，并在响应刷新后关闭连接。若对端在帧尚不完整时关闭，
服务器直接关闭连接。

`not_found`、`invalid_key`、`item_too_large` 等格式合法的缓存错误返回各自状态码，
不会关闭连接。
