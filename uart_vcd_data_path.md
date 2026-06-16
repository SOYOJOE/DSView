# UART_VCD 当前数据通路与内存分析

## 1. 当前架构

UART_VCD 使用事件协议，不再从串口接收每个采样点的 32-bit GPIO 状态。

```text
Telink MCU
  -> protocol v3 event stream
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
| RX0-RX7 | v3 direct text 直接显示 annotation，不生成 RX 波形 |

MCU 的 24 MHz `stimer_get_tick()` 与 DSView 的 24 MHz sample 一一对应。

## 2. Protocol v3

每个事件以 2-byte magic 加 3-byte little-endian delta tick 开头：

```text
[0xA5][0x5A][delta_ticks:3][header:1][optional payload]
```

GPIO 事件固定为 6 bytes：

```text
[0xA5][0x5A][delta:3][header:1]
```

direct text 事件为：

```text
label: [0xA5][0x5A][delta:3][0x80][channel:1][label_len:1][label][padding]
text:  [0xA5][0x5A][delta:3][0xC0/0xE0][channel:1][data_len:1][data][padding]
```

`A5 5A` 用于 PC 端重同步，避免 text payload 中的普通字节被误判为 GPIO
事件。

PC 端通过 `SR_DF_UART_VCD_TEXT` 直接推入 decoder annotation row，不再为
RX0-RX7 合成 8N1 波形。完整定义见 `test_uart_vcd_event_protocol_v3.md`。

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

`ev3_blow_buf()` 完成一个事件的解析：

1. 校验 `A5 5A` sync word，读取 24-bit delta。
2. 将绝对采样时间推进 delta，但不生成空闲采样。
3. GPIO 事件再更新 GPIO 状态。
4. text 事件直接发送 `SR_DF_UART_VCD_TEXT` annotation packet。

首事件 delta 被替换为 1 sample，异常大 delta 会被限制：

| 限制 | 当前值 |
|---|---:|
| 单事件最大 delta | 16,777,215 samples |
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

GPIO 每通道每秒 toggle 1,000 次，并同时让 RX0-RX7 每毫秒发送 5-byte
direct text payload：

```bash
python3 send_event_test.py gpio-uart-1k \
  --payload-mbps 2.1 --duration 10
```

该场景每毫秒固定产生 256 bytes protocol payload：

```text
24 GPIO events * 6 bytes + 8 text events * 14 bytes = 256 bytes/ms
```

即 2.048 Mbps TCP payload；如果按物理 8N1 串口计算，需要 2.56 Mbaud。
驱动每 0.25 秒逻辑时间输出一次 `activity` 掩码。v3 direct text 不再生成
RX0-RX7 波形，所以正常复合测试的 GPIO activity 应为 `0x00ffffff`；RX 文本
是否进入 UI 需要看 `uart_vcd text annotation...` 日志。

解析器会校验事件 header、文本长度和对齐 padding。收到非法帧时打印原始
十六进制数据，向后寻找可连续解析的事件边界，丢弃错帧后继续采集。RX FIFO
或输入缓冲溢出也只报告并丢弃受影响的数据，不再发送会终止 UI 会话的
`SR_DF_OVERFLOW`。

重新同步优先使用完整 label/text event 作为强边界。

稀疏事件包的 UI 接收进度按 `LogicSnapshot` 实际增加的 sample 数计算，不再
把 16-byte sparse record 错当成普通 32 通道位图数据。
### 3.3 LA_SPARSE_EVENTS 输出

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

### 7.2 已实现：v3 direct text

RX0-RX7 文本事件直接进入 annotation row，不再合成 8N1 波形，也不再运行
UART decoder。UART decoder stack 仅作为当前 UI 的 annotation row 容器，decode
worker 会直接跳过，避免停止采集后清空 direct annotations。

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

protocol v3 本身已经是压缩事件流。最有效方案是让上层直接保存：

```text
GPIO: (sample_index, new_level)
RX:   (sample_index, direct text annotation)
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

### 7.6 Protocol v3 评估

当前 protocol v3 的 GPIO event 固定 6 bytes：

```text
magic16 + uint24 delta + header(channel/sub-mode)
```

对 24 MHz timer 来说，1 ms delta 是 24,000 ticks，仍需要 3 bytes。改成
varint 只能在更短 delta 下省 1-2 bytes，但会增加 MCU/PC 分支和重同步复杂度。
因此在当前算力优先的前提下，GPIO fixed uint24 是合理折中。

主要可优化点在 text/RX：

- HEX render 会把每个 data byte 变成两个 ASCII 字符；能用 ASCII 时应优先用
  ASCII。
- label 只在启动时作为 metadata 发送，后续 text event 只带 data。
- `gpio_event_send_text()` 的 `data_len` 是 8-bit，单帧 data 最大 255 bytes。

在保持相同算力和带宽的情况下，优先级最高的是减少 text payload 字节数，
而不是改 GPIO delta 编码。

### 7.7 v3 direct text（已实现最小闭环）

当前已实现：

- MCU `gpio_event_send_label()` / `gpio_event_send_text()`。
- PC `ev3_blow_buf()` 解析 `0x80` label、`0xC0` HEX text、`0xE0` ASCII text。
- 新增 `SR_DF_UART_VCD_TEXT`，由 `SigSession` 按 RX channel 路由到对应
  decoder stack。
- direct annotation UI 刷新按 256 条节流。
- v2 parser 已删除；v3 是唯一支持协议。

未实现：

- mode 2 multi-GPIO mask。
- v3 专用 TCP 测试发送器。
- 不依赖 UART decoder 的专用 text row。

### 7.8 可选 v3 后续方向

如果需要在相同 MCU 算力和物理带宽下继续提高上报量，建议新增可选 v3 event：

1. **Label dictionary / metadata**：每个 RX channel 的 label 只发送一次，后续
   event 只带 data。
2. **multi-string group**：同一 delta 下用 `rx_mask` 聚合 RX0-RX7 的多个
   payload，避免重复 4-byte delta/header、channel/length 和 padding。
3. **multi-GPIO mask**：同一 tick 多个 GPIO 同时变化时，用 `changed_mask`
   和 `level_mask` 一次描述。24 路同时变化可从 96 bytes 降到约 10 bytes。
4. **专用 text row**：替代借用 UART decoder stack 作为 annotation row 容器。

### 7.9 Leaf block 常量状态压缩

折中方案是在 `LogicSnapshot` 中将全 0/全 1 的 leaf block 表示为常量标记，
仅在块内出现边沿时分配 2.03 MiB 数据。它能显著压缩长期静止的 GPIO，
且保留现有查询接口。

限制是当前写入过程会在 leaf block 尚未完成时立即分配 dense buffer。要获得
收益，需要增加按块事件构建或延迟物化机制。

## 8. 推荐顺序

1. 保持当前 loop sparse prune 和 v3 direct text。
2. MCU 侧约束 `data_len <= 255`、`render_mode <= 1`，避免无效帧。
3. 高频字符串尽量用 ASCII，并减少重复 label。
4. 评估真实 channel enable，减少无关 UI 和 decoder 工作。
5. 若 RX0-RX7 文本量继续增长，优先做 direct text annotation 或 v3
   multi-string group，而不是优先改 GPIO uint24 delta。
6. 不建议单独照搬 DSL 的 `SR_CONF_RLE`，因为 DSLogic 路径最终仍要求
   dense sample data。
