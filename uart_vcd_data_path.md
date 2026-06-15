# UART_VCD 当前数据通路与内存分析

## 1. 当前架构

UART_VCD 使用事件协议，不再从串口接收每个采样点的 32-bit GPIO 状态。

```text
Telink MCU
  -> protocol v2 event stream
  -> UART 3 Mbps
  -> low_part/bridge/serial_bridge.py
  -> TCP server :12345
  -> libsigrok4DSL/hardware/uart_vcd/uart_vcd.c
  -> 24 MHz dense LA_CROSS_DATA
  -> SigSession
  -> LogicSnapshot
  -> waveform view and protocol decoders
```

通道分配：

| 通道 | 用途 |
|---|---|
| D0-D23 | GPIO high/low/toggle 事件 |
| RX0-RX7 | 字符串事件在 PC 端合成的 8 路 UART RX 波形 |

MCU 的 24 MHz `stimer_get_tick()` 与 DSView 的 24 MHz sample 一一对应。

## 2. Protocol v2

每个事件以 3-byte little-endian delta tick 开头：

```text
[delta_ticks:3][header:1][optional payload]
```

GPIO 事件固定为 4 bytes：

```text
[delta:3][header:1]
```

字符串事件为：

```text
[delta:3][header:1][channel:1][total_len:1][label][data][padding]
```

`channel` 范围为 0-7，对应 RX0-RX7。字符串 payload 从 `channel`
开始按 4 bytes 对齐。完整定义见 `test_uart_vcd_event_protocol_2.md`。

## 3. PC 端处理

### 3.1 TCP 接收

- 驱动连接 `tcp_host:12345`，默认 host 当前定义在 `uart_vcd.h`。
- socket 在连接阶段和采集阶段均为 non-blocking。
- 单次读取缓冲为 64 KiB，累积输入缓冲为 1 MiB。
- 缓冲区溢出时丢弃旧的未解析数据，保留最新一次读取。

### 3.2 事件展开

`ev2_blow_buf()` 完成一个事件的解析：

1. 读取 24-bit delta。
2. 先输出 delta 对应的旧状态采样。
3. GPIO 事件再更新 GPIO 状态。
4. 字符串事件加入对应 RX 通道的 64-byte FIFO。

首事件 delta 被替换为 1 sample，异常大 delta 会被限制：

| 限制 | 当前值 |
|---|---:|
| 单事件最大 delta | 12,000,000 samples |
| 单回调最大事件数 | 2,048 |
| 单回调最大展开采样数 | 3,145,728 |

### 3.3 RX0-RX7 UART 合成

每个字符生成标准 8N1 波形：

```text
start(0) + 8 data bits LSB-first + stop(1)
```

当前虚拟 UART 波特率为：

```text
UART_VCD_UART_BAUD_RATE = 24 MHz / 4 = 6 Mbaud
```

因此每 bit 为 4 samples。物理 MCU 到 bridge 的 3 Mbps UART 与这里的
虚拟 6 Mbaud 波形是两个独立参数。

HEX 模式将一个 data byte 转成两个大写 ASCII hex 字符；ASCII 模式直接
发送 data byte。label 始终作为 ASCII 字节发送。

### 3.4 LA_CROSS_DATA 输出

驱动将事件展开为 DSView 现有逻辑数据格式：

```text
64 samples x 32 channels = 256 bytes
```

每个通道占连续 8 bytes，每个 bit 是一个采样点，LSB 表示较早采样。
最多批量聚合 256 个 chunk，即 65,536 bytes，再通过 `ds_data_forward()`
发送 `SR_DF_LOGIC`。

## 4. LogicSnapshot 存储

`LogicSnapshot` 只接受 dense `LA_CROSS_DATA`。每个通道分别保存完整位图，
并生成三级 mipmap 供波形边沿查询。

关键参数：

| 参数 | 值 |
|---|---:|
| LeafBlockSamples | 16,777,216 samples |
| LeafBlockSpace | 2,130,440 bytes/channel |
| 32 通道一组 leaf block | 68,174,080 bytes |
| 一组覆盖时间（24 MHz） | 0.699 s |

LeafBlockSpace 包括原始位图和 mipmap：

```text
(64 + 64^2 + 64^3 + 64^4) / 8 = 2,130,440 bytes
```

## 5. 内存增长原因

32 通道、24 MHz 的原始位图速率：

```text
24,000,000 samples/s * 32 channels / 8 = 96,000,000 bytes/s
```

mipmap 的额外比例：

```text
1/64 + 1/64^2 + 1/64^3 = 1.5873%
```

所以 LogicSnapshot 的理论增长约为：

```text
96,000,000 * 1.015873 = 97,523,804 bytes/s
```

这就是每秒接近 100 MB 的主要来源。驱动自身长期缓冲约为：

| 缓冲 | 大小 |
|---|---:|
| input_buf | 1 MiB |
| output_buf | 256 bytes |
| batch_buf | 64 KiB |
| TCP receive buffer | 512 KiB（内核） |

因此缩小 `uart_vcd.c` 的 batch 或 input buffer 不能显著降低总内存。

## 6. DSL RLE 是否可复用

DSL 的 `SR_CONF_RLE` 是硬件/FPGA 采集能力。`dsl_start_transfers()` 将 RLE
配置写入 FPGA，但 `receive_transfer()` 最终仍向 Session 提交普通
`LA_CROSS_DATA`。`LogicSnapshot` 没有 RLE packet 格式，也不会保存压缩流。

因此只给 UART_VCD 增加 `SR_CONF_RLE` 开关没有效果。即使驱动内部使用 RLE，
在调用 `ds_data_forward()` 前展开成 `LA_CROSS_DATA`，Snapshot 内存仍然约
97.5 MB/s。

## 7. 可行优化

### 7.1 启用真实的通道开关

UART_VCD 当前对 `SR_CONF_PROBE_EN` 总是返回 true，并忽略 set。Snapshot
只为 enabled channel 分配数据，因此支持关闭未使用通道可近似线性节省内存：

```text
每个 enabled channel约 3.05 MB/s
```

这是低风险、低侵入的第一步，但 32 通道全开时没有收益。驱动输出布局必须
同时改为只包含 enabled channel，否则 Snapshot 会按错误通道数解释 packet。

### 7.2 限制 loop mode 时间窗口

现有 LogicSnapshot 已支持按 `total_samples` 循环释放旧 leaf block。设置
固定保留时间可以把内存从持续增长变成固定上限：

| 保留时间 | 32 通道估算内存 |
|---|---:|
| 1 s | 约 98 MB |
| 5 s | 约 488 MB |
| 10 s | 约 975 MB |

此方案不降低每秒写入带宽，但适合实时观察。

### 7.3 Event-native sparse snapshot

protocol v2 本身已经是压缩事件流。最有效方案是让上层直接保存：

```text
GPIO: (sample_index, new_level)
RX:   (sample_index, byte stream/render mode)
```

波形绘制可直接从边沿列表生成，内存取决于事件数量而不是采样率。对于长空闲
GPIO，压缩比可达到数千倍。

主要改动点：

- 扩展 `sr_datafeed_logic` 或增加新的 event packet 类型。
- 新增 sparse snapshot，或为 `LogicSnapshot` 增加 event backend。
- 实现 `get_sample()`、边沿查询、循环窗口和保存功能。
- 协议解码器需要按需将指定时间窗口展开成 dense sample block。

这是推荐的长期方案，但涉及 Session、Snapshot、decoder 和文件保存边界。

### 7.4 Leaf block 常量状态压缩

折中方案是在 `LogicSnapshot` 中将全 0/全 1 的 leaf block 表示为常量标记，
仅在块内出现边沿时分配 2.03 MiB 数据。它能显著压缩长期静止的 GPIO，
且保留现有查询接口。

限制是当前写入过程会在 leaf block 尚未完成时立即分配 dense buffer。要获得
收益，需要增加按块事件构建或延迟物化机制。RX 通道持续产生串行边沿时仍会
使用完整块。

## 8. 推荐顺序

1. 支持真实 channel enable，降低未使用通道成本。
2. 为实时模式设置明确的 ring window，阻止无限增长。
3. 实现 event-native sparse snapshot，并为 decoder 按需展开。
4. 不建议单独照搬 DSL 的 `SR_CONF_RLE`，因为当前 Session/Snapshot 接口
   最终仍要求 dense sample data。
