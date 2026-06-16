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
  -> LA_SPARSE_EVENTS
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

### 3.1.1 MCU UART/DMA 发送

- 字符串事件先完整写入 ping-pong buffer，最后一次性提交长度，DMA 不会看到
  半帧。
- DMA busy 时仍可写另一个 4 KiB buffer；空间不足时整帧丢弃，不再拆成 header
  和 payload 两次发送。
- TXDONE ISR 只清除 busy 状态，不在中断中切换发送 buffer。
- `gpio_event_irq_high/low/toggle()` 是 GPIO ISR 专用快速接口，不检查通道，也不
  在 GPIO ISR 内启动 DMA；主循环继续调用 `uart_tx_poll()`。

### 3.2 事件调度

`ev2_blow_buf()` 完成一个事件的解析：

1. 读取 24-bit delta。
2. 将绝对采样时间推进 delta，但不生成空闲采样。
3. GPIO 事件再更新 GPIO 状态。
4. 字符串事件加入对应 RX 通道的 64-byte FIFO。

首事件 delta 被替换为 1 sample，异常大 delta 会被限制：

| 限制 | 当前值 |
|---|---:|
| 单事件最大 delta | 12,000,000 samples |
| 单回调最大事件数 | 65,536 |

event-native backend 不再按虚拟采样数限制单次回调。旧的 3,145,728 sample
预算会在长 delta 或高通道数测试中造成 socket 积压，现已移除。

多通道频率测试中的 delta 是相邻任意两个全局事件之间的间隔，不是单通道
周期。24 路方波、每通道 1 kHz 时：

```text
delta = 24 MHz / (24 channels * 2 edges * 1 kHz) = 500 ticks
```

可使用：

```bash
python3 send_event_test.py gpio-frequency \
  --gpio-channels 24 --gpio-frequency 1000 \
  --wire-mbps 2.1
```

GPIO 每通道每秒 toggle 1,000 次，并同时让 RX0-RX7 每毫秒发送与固件一致的
11-byte string payload（`"lable:"` 6 bytes + data 5 bytes）：

```bash
python3 send_event_test.py gpio-uart-1k \
  --payload-mbps 2.1 --duration 10
```

该场景每毫秒固定产生 256 bytes protocol payload：

```text
24 GPIO events * 4 bytes + 8 UART events * 20 bytes = 256 bytes/ms
```

即 2.048 Mbps TCP payload；如果按物理 8N1 串口计算，需要 2.56 Mbaud。
RX0-RX3 使用 HEX render，最终各生成 16 个虚拟 UART 字符；RX4-RX7 使用
ASCII render，各生成 11 个虚拟 UART 字符。
驱动每 0.25 秒逻辑时间输出一次 `activity` 掩码。正常复合测试应为
`activity=0xffffffff`，否则可直接定位是 GPIO 还是 RX 通道未产生边沿。

解析器会校验事件 header、字符串长度和对齐 padding。收到非法帧时打印原始
十六进制数据，向后寻找可连续解析的事件边界，丢弃错帧后继续采集。RX FIFO
或输入缓冲溢出也只报告并丢弃受影响的数据，不再发送会终止 UI 会话的
`SR_DF_OVERFLOW`。

重新同步优先使用完整 string event 作为强边界。若 MCU 数据中只缺少 string
event 的 3-byte delta，但 header、payload 和 padding 完整，PC 会使用零 delta
恢复该字符串并继续解析；无法确定边界的 GPIO 字节不会被猜测为有效事件。

稀疏事件包的 UI 接收进度按 `LogicSnapshot` 实际增加的 sample 数计算，不再
把 16-byte sparse record 错当成普通 32 通道位图数据。
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

### 3.4 LA_SPARSE_EVENTS 输出

驱动向 Session 提交事件记录：

```text
struct sr_logic_sparse_event {
    uint64_t sample;   // absolute 24 MHz sample index
    uint32_t state;    // D0-D23 + RX0-RX7
    uint32_t reserved;
}
```

只有状态变化和批次末尾时间点会产生记录。最多聚合 4,096 条记录，
再通过 `ds_data_forward()` 发送 `SR_DF_LOGIC`。

## 4. LogicSnapshot 存储

`LogicSnapshot` 同时保留 DSLogic dense backend 和 UART_VCD sparse backend。
UART_VCD 每通道保存 `(sample, level)` 边沿数组，单点和前后边沿查询使用
二分查找。

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

## 5. 内存模型

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

这是旧 dense backend 每秒接近 100 MB 的原因。当前 UART_VCD 不再分配这部分
长期位图。每个 `SparseEdge` 当前通常占 16 bytes，因此近似为：

```text
memory ~= edge_count * 16 bytes + vector capacity
```

驱动自身长期缓冲约为：

| 缓冲 | 大小 |
|---|---:|
| input_buf | 1 MiB |
| event_buf | 64 KiB |
| TCP receive buffer | 512 KiB（内核） |

因此缩小 `uart_vcd.c` 的 batch 或 input buffer 不能显著降低总内存。

## 6. DSL RLE 是否可复用

DSL 的 `SR_CONF_RLE` 是硬件/FPGA 采集能力。`dsl_start_transfers()` 将 RLE
配置写入 FPGA，但 `receive_transfer()` 最终仍向 Session 提交普通
`LA_CROSS_DATA`。`LogicSnapshot` 没有 RLE packet 格式，也不会保存压缩流。

因此只给 UART_VCD 增加 `SR_CONF_RLE` 开关没有效果。即使驱动内部使用 RLE，
在调用 `ds_data_forward()` 前展开成 `LA_CROSS_DATA`，Snapshot 内存仍然约
97.5 MB/s。

## 7. 当前实现和后续优化

### 7.1 已实现：loop sparse prune 节流

loop mode 达到最大 sample 窗口后，旧边沿需要从 sparse edge vector 中淘汰。
如果每个输入包都执行 `vector::erase()`，到 1.74 min 左右的最大 sample 后，
CPU 会从正常采集占用突增到 100%，UI 停止响应。

当前实现只在 `loop_offset` 至少推进约 1 秒 sample 后执行一次 sparse prune：

```text
prune_step = max(samplerate, LeafBlockSamples)
```

正常日志示例：

```text
DSView: LogicSnapshot sparse prune: loop_offset=... elapsed=... ms
```

这使 loop 5 s、10 s 以及超过最长采样窗口后的 CPU 占用保持稳定。

### 7.2 已实现：采集中 defer native UART decode

RX0-RX7 的字符串事件会被 PC 合成 8N1 UART 波形。如果采集同时运行 native
UART decode，8 路 decoder 会和采集、UI、snapshot 写入抢 CPU，并且 UI 上的
解释结果也要等解码结果进入对应 row 后才能显示。

当前策略：

- 采集进行中，native UART decode 直接返回 defer。
- single/loop 停止或采集自然结束后，再对已有 sparse snapshot 做 native decode。
- 解码线程仍可并行处理多个 RX 通道，但不会拖慢采集主路径。

这适合当前 UART_VCD 的主要目标：先稳定采全量数据，再显示 UART 文本。

### 7.3 启用真实的通道开关

UART_VCD 当前对 `SR_CONF_PROBE_EN` 总是返回 true，并忽略 set。Snapshot
只为 enabled channel 分配数据，因此支持关闭未使用通道可近似线性节省内存：

```text
每个 enabled channel约 3.05 MB/s
```

这是低风险、低侵入的第一步，但 32 通道全开时没有收益。驱动输出布局必须
同时改为只包含 enabled channel，否则 Snapshot 会按错误通道数解释 packet。

### 7.4 限制 loop mode 时间窗口

现有 LogicSnapshot 已支持按 `total_samples` 循环释放旧 leaf block。设置
固定保留时间可以把内存从持续增长变成固定上限：

| 保留时间 | 32 通道估算内存 |
|---|---:|
| 1 s | 约 98 MB |
| 5 s | 约 488 MB |
| 10 s | 约 975 MB |

此方案不降低每秒写入带宽，但适合实时观察。

### 7.5 Event-native sparse snapshot（已实现）

protocol v2 本身已经是压缩事件流。最有效方案是让上层直接保存：

```text
GPIO: (sample_index, new_level)
RX:   (sample_index, byte stream/render mode)
```

波形绘制可直接从边沿列表生成，内存取决于事件数量而不是采样率。对于长空闲
GPIO，压缩比可达到数千倍。

当前已经完成：

- `LA_SPARSE_EVENTS` 和 `sr_logic_sparse_event`。
- `LogicSnapshot` sparse backend、边沿查询和循环窗口。
- 保存/导出按块临时物化。
- decoder 按下一个真实边沿分块；无边沿区间传 constant channel，由
  libsigrokdecode 直接跳过。

因此采集和解码 CPU 应主要取决于 TCP 事件率及真实边沿率，而不再取决于
24 MHz 虚拟采样率。

### 7.6 Protocol v2 评估

当前 protocol v2 没有明显的 GPIO 编码问题。GPIO event 固定 4 bytes：

```text
uint24 delta + header(channel/sub-mode)
```

对 24 MHz timer 来说，1 ms delta 是 24,000 ticks，仍需要 3 bytes。改成
varint 只能在更短 delta 下省 1-2 bytes，但会增加 MCU/PC 分支和重同步复杂度。
因此在当前算力优先的前提下，GPIO fixed uint24 是合理折中。

主要可优化点在 string/RX：

- HEX render 会把每个 data byte 变成两个 ASCII 字符；能用 ASCII 时应优先用
  ASCII。
- 当前高频测试每个 RX event 都重复 `"lable:"`。如果 label 只在启动时作为
  metadata 发送，后续 event 使用 `label_len=0`，可同时节省串口带宽、PC
  事件解析和虚拟 UART 边沿。
- `gpio_event_send_string()` 的 `total_len` 是 8-bit。调用方必须保证
  `label_len + data_len <= 255`，且 `render_mode` 只能是 HEX/ASCII。否则会
  出现长度回绕或 PC parser 拒帧。
- PC 端每个 RX 通道的虚拟 UART FIFO 为 64 bytes。单个 string event 渲染后
  超过 FIFO 或突发过快时，会丢 RX 字节并打印 overflow。

在保持相同算力和带宽的情况下，优先级最高的是减少“要被合成并解码的 UART
字符数”，而不是改 GPIO delta 编码。

### 7.7 可选 v3 协议方向

如果需要在相同 MCU 算力和物理带宽下继续提高上报量，建议新增可选 v3 event，
并保留 v2 兼容模式：

1. **Label dictionary / metadata**：每个 RX channel 的 label 只发送一次，后续
   event 只带 data。
2. **multi-string group**：同一 delta 下用 `rx_mask` 聚合 RX0-RX7 的多个
   payload，避免重复 4-byte delta/header、channel/length 和 padding。
3. **multi-GPIO mask**：同一 tick 多个 GPIO 同时变化时，用 `changed_mask`
   和 `level_mask` 一次描述。24 路同时变化可从 96 bytes 降到约 10 bytes。
4. **direct text annotation**：RX 字符串直接进入 annotation row，不再合成
   8N1 波形，也不再运行 UART decoder。这是 PC CPU 收益最大的方案，但会牺牲
   默认 bit-level UART 波形；可作为“文本优先模式”。

### 7.8 Leaf block 常量状态压缩

折中方案是在 `LogicSnapshot` 中将全 0/全 1 的 leaf block 表示为常量标记，
仅在块内出现边沿时分配 2.03 MiB 数据。它能显著压缩长期静止的 GPIO，
且保留现有查询接口。

限制是当前写入过程会在 leaf block 尚未完成时立即分配 dense buffer。要获得
收益，需要增加按块事件构建或延迟物化机制。RX 通道持续产生串行边沿时仍会
使用完整块。

## 8. 推荐顺序

1. 保持当前 loop sparse prune 和采集中 defer native UART decode。
2. MCU 侧约束 `label_len + data_len <= 255`、`render_mode <= 1`，避免无效帧。
3. 高频字符串尽量用 ASCII，并减少重复 label。
4. 评估真实 channel enable，减少无关 UI 和 decoder 工作。
5. 若 RX0-RX7 文本量继续增长，优先做 direct text annotation 或 v3
   multi-string group，而不是优先改 GPIO uint24 delta。
6. 不建议单独照搬 DSL 的 `SR_CONF_RLE`，因为 DSLogic 路径最终仍要求
   dense sample data。
