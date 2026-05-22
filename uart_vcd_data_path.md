# UART_VCD 数据通路说明

## 概述

uart_vcd 驱动程序从串口接收 32bit 原始数据，将每一个 bit 映射为一个逻辑分析通道，
转换后的数据经 libsigrok 数据总线 → Snapshot 缓冲层 → UI 渲染管线，最终在屏幕上
以 32 通道逻辑波形呈现。

```
串口原始字节 → [接收缓冲] → [pack_output_block 位打包] → [SR_DF_LOGIC 数据包]
    → [data_feed_callback 分发] → [LogicSnapshot 叶子块存储] → [UI 渲染]
```

---

## 1. 串口输入层

### 1.1 串口配置

| 参数 | 值 | 说明 |
|------|-----|------|
| 波特率 | 115200 (默认) | 可通过 config_set 修改 |
| 数据位 | 8 | cfmakeraw 默认 |
| 停止位 | 1 | cfmakeraw 默认 |
| 校验位 | 无 | cfmakeraw 默认 |
| 流控 | 无 | cfmakeraw 默认 |
| 读取模式 | VMIN=0, VTIME=1 | 最多阻塞 100ms |
| 打开标志 | O_RDWR \| O_NOCTTY | 非阻塞轮询，由 G_IO_IN 驱动 |

### 1.2 单次 32bit 采样格式

每个完整的 32bit 采样由连续的 4 个串口字节组成，**小端序 (Little-Endian)**：

```
串口字节序列: Byte0, Byte1, Byte2, Byte3
32bit 重构值: Byte0 | (Byte1 << 8) | (Byte2 << 16) | (Byte3 << 24)

位映射:  bit[0]  → D0 通道
         bit[1]  → D1 通道
         bit[2]  → D2 通道
         ...
         bit[31] → D31 通道
```

示例：串口收到 `0x03 0x00 0x00 0x80`

```
重构值 = 0x03 | (0x00 << 8) | (0x00 << 16) | (0x80 << 24)
       = 0x80000003

此时通道状态:
  D0  = 1  (bit0  = 1)
  D1  = 1  (bit1  = 1)
  D2  = 0  (bit2  = 0)
  ...
  D31 = 1  (bit31 = 1)
```

---

## 2. 驱动层数据转换

### 2.1 采集启动 (hw_dev_acquisition_start)

```
1. 重置 collected_samples = 0, collecting = TRUE
2. 检查串口 fd 有效性，必要时重新打开
3. 分配 input_buf (64KB) 和 output_buf (256 bytes)
4. 注册 G_IO_IN 事件源：sr_session_source_add(fd, G_IO_IN, 100ms, receive_data)
```

### 2.2 数据接收与累积 (receive_data)

```
每次 G_IO_IN 触发：
  1. read(fd, read_buf, 64KB) → 读原始字节
  2. memcpy 到 ctx->input_buf 尾部，累加 input_len
  3. 当 input_len >= 256 bytes 时，进入转换循环
```

### 2.3 位打包 — pack_output_block (核心转换)

这是将 64 个 32bit 采样（256 字节串口原始数据）转换为一帧 LA_CROSS_DATA
格式输出的过程。

**关键常量：**
```c
UART_VCD_UART_BYTES_PER_SAMPLE = 4    // 每个采样 4 字节 (32bit)
UART_VCD_SAMPLES_PER_OUTPUT    = 64   // 每帧 64 个时间采样
UART_VCD_NUM_PROBES            = 32   // 通道数
UART_VCD_OUTPUT_SIZE           = 256  // 输出帧大小 = 32×8 = 256 bytes
```

**转换算法（伪代码）：**
```
输入: input_buf[0..255] — 64个32bit采样 × 4字节/采样

对于每个通道 ch (0..31):
  对于每个输出字节 b (0..7):      // 8字节 = 64个采样点
    令输出字节 = 0
    对于每个 bit 位置 s (0..7):   // 每个字节 8 个采样点
      sample_idx = b * 8 + s      // 采样序号 0..63
      sample_ptr = input_buf + sample_idx * 4
      32bit_sample = sample_ptr[0] | (ptr[1] << 8) | (ptr[2] << 16) | (ptr[3] << 24)
      若 (32bit_sample & (1 << ch)):   // 该采样中通道 ch 为高
        byte_val |= (1 << s)           // 在输出字节中置对应位
    输出[ch * 8 + b] = byte_val
```

**输出缓冲区排布 (256 bytes)：**

| 偏移 | 内容 | 说明 |
|------|------|------|
| 0..7 | D0 通道 | 8字节 = 64个时间采样点 |
| 8..15 | D1 通道 | 8字节 = 64个时间采样点 |
| 16..23 | D2 通道 | ... |
| ... | ... | ... |
| 248..255 | D31 通道 | 8字节 = 64个时间采样点 |

每 1 个字节 = 连续的 8 个时间采样点（同一通道），LSB 为较早采样，MSB 为较晚采样。

示例：D0 通道输出字节[0] = 0x55 = 0b01010101

```
bit0(S0)  bit1(S1)  bit2(S2)  bit3(S3)  bit4(S4)  bit5(S5)  bit6(S6)  bit7(S7)
   1         0         1         0         1         0         1         0

采样 S0: D0=1, 采样 S1: D0=0, 采样 S2: D0=1 ... 波形呈现 01010101 翻转
```

### 2.4 输出数据包 (SR_DF_LOGIC)

```c
packet.type       = SR_DF_LOGIC
packet.payload    = &logic
logic.format      = LA_CROSS_DATA    // 通道优先排列
logic.index       = 0
logic.order       = 0
logic.length      = 256              // 输出帧字节数
logic.unitsize    = 1                // 1 字节 = 8 个采样点
logic.data_error  = 0
logic.error_pattern = 0
logic.data        = output_buf       // 256 字节输出帧
```

### 2.5 停止条件

- **非 Loop 模式**: `collected_samples >= total_samples` → 发送 SR_DF_END → `collecting = FALSE`
- **Loop 模式**: 永不自动停止，持续转发数据，由 UI 用户手动停止

---

## 3. Session 数据总线

### 3.1 数据分发 (data_feed_callback)

```
ds_data_forward(sdi, packet)
  └→ 回调注册的 data_feed_callback (sigsession.cpp:1480)
      └→ case SR_DF_LOGIC: feed_in_logic() → 第一帧/后续帧分发
```

### 3.2 第一帧 vs 后续帧

```
第一帧 (last_ended == true):
  first_payload(o, sample_limit, channels, bNotFree)
    ├→ init() 清空旧数据
    ├→ 设置 _total_sample_count, _channel_num
    ├→ 创建各通道叶子块索引
    └→ append_cross_payload(o)  // 存储第一帧数据

后续帧:
  append_payload(o) → append_cross_payload(o)

终止帧 (SR_DF_END):
  capture_ended() → _last_ended = true
  └→ 触发解码线程，UI 更新结束状态
```

---

## 4. LogicSnapshot 存储格式

### 4.1 关键常量

```
ScalePower  = 6
Scale       = 64     (1<<6)
ScaleSize   = 8      (Scale/8)
ScaleLevel  = 4
LeafBlockPower = 24  (ScalePower × ScaleLevel)
LeafBlockSamples = 16,777,216  (1<<24)
```

### 4.2 数据存储架构

```
_ch_data[32][N]              ← 32 个通道，每个通道 N 个 RootNode
  └─ RootNode
       ├─ tog: uint64_t      ← 边沿检测标志（仅根节点层级）
       ├─ first: uint64_t    ← 最近数据块首字
       ├─ last: uint64_t     ← 最近数据块尾字
       └─ lbp[64]: void*     ← 64 个叶子块指针

叶子块:
  每个叶子块 = LeafBlockSpace bytes (约 1.18 MB)
  可存储 LeafBlockSamples (16M) 个采样点
  存储层级: [L0] 8B = 64bit 每层 → [L1] 聚合 64x → [L2] 聚合 64x → [L3]
```

### 4.3 输入数据注入 (append_cross_payload)

```
输入: 256 bytes LA_CROSS_DATA (32通道 × 8字节/通道)

位对齐阶段 (处理上一帧未对齐的碎片):
  逐字节拷贝至对应通道叶子块
  _byte_fraction: 该通道当前字节偏移 (0..7)，每次 +1 模 8
  _ch_fraction: 当前通道索引 (0..31)，每次字节满 8 时 +1 模 32

批量拷贝阶段 (字节对齐后):
  while (len >= 8):
    64bit 贪心读取: *read_ptr++ = *write_ptr  (Scale=64 个采样/次)
    read_ptr 跨通道跳转: read_ptr += _channel_num (32)
    每处理一个通道: last_chan++
    每处理完一轮所有 32 通道: filled_sample += Scale (64)
    
    当 filled_sample == LeafBlockSamples (16M):
      计算 mipmap (calc_mipmap)
      切换到下一通道的叶子块

64bit 存储单元格式:
  每 64bit 存储 64 个连续的采样点（同一通道）
  MSB → 较晚采样
  LSB → 较早采样
```

### 4.4 叶子块内排布

```
叶子块[通道 K]:
  字节 0..7:   采样 0..63     (Scale=64 采样点)
  字节 8..15:  采样 64..127
  字节 16..23: 采样 128..191
  ...
  读/写指针: (uint64_t*)(lbp + offset/Scale)，每次 +1 = Scale(64)个采样
```

---

## 5. UI 渲染链路

```
get_display_edges(start, end, width, max_togs, pixels_offset, min_length, sig_index)
  → 基于像素宽度 + 时间范围，从多级 mipmap 层级中选最佳层级
  → 查找边沿位置 (get_nxt_edge)
  → 返回 EdgePair 列表 { <采样索引, 边状态(上升/下降)>, ... }
  → 每条边对应 UI 中的一个水平像素位置
  → QPainter 渲染为矩形波（垂直翻转边）
```

---

## 6. 端到端数据流示例

### 假设：串口连续收到 4 字节/采样 × 128 个采样

```
串口原始数据 (512 bytes):
  S0:  0x01 0x00 0x00 0x00  → D0=1
  S1:  0x02 0x00 0x00 0x00  → D1=1
  S2:  0x04 0x00 0x00 0x00  → D2=1
  S3:  0x08 0x00 0x00 0x00  → D3=1
  ... (64 个采样后 input_buf 累积 256 bytes，触发 pack)

pack_output_block 输出 (256 bytes):
  D0[0..7]:  0xFF 0x00 0x00 ... → 前8个采样 D0=全部高，后56个=全部低
  D1[0..7]:  0x00 0xFF 0x00 ... → 采样8..15 D1=高
  D2[0..7]:  0x00 0x00 0xFF ... → 采样16..23 D2=高
  ...

ds_data_forward → SR_DF_LOGIC → data_feed_callback

LogicSnapshot:
  samples = ceil(256 * 8 / 32) = 64 采样
  D0: byte0 存入 lbp offset 0 (采样0..63)
  D1: byte0 存入 lbp offset 0

后续 64 个采样 → 第二帧 256 bytes → append_cross_payload → D0..D31 各追加 64 采样

UI 渲染:
  用户视野 = (start=0, end=64) → 请求 D0 边沿
  get_display_edges → 识别 D0 在采样0处上升（bit[0] = 1）
  → 渲染像素位置 0 处显示上升沿 + 高电平
```

---

## 7. 调试入口

| 调试目标 | 位置 | 方法 |
|---------|------|------|
| 串口数据接收 | uart_vcd.c:558-572 | 打印 read() 返回值 n 和原始字节 |
| 位打包输出 | uart_vcd.c:574-599 | 打印 output_buf 的十六进制内容 |
| Session 分发 | sigsession.cpp:1416-1419 | 断点在 SR_DF_LOGIC case |
| Snapshot 存储 | logicsnapshot.cpp:215-400 | 跟踪 append_cross_payload 分支 |
| UI 渲染请求 | logicsnapshot.cpp:113-117 | 跟踪 get_display_edges 参数 |

---

## 8. 数据尺寸速查

| 量 | 值 | 公式 |
|----|-----|------|
| 单采样串口字节 | 4 | UART_VCD_UART_BYTES_PER_SAMPLE |
| 每帧采样数 | 64 | UART_VCD_SAMPLES_PER_OUTPUT |
| 每帧串口输入 | 256 bytes | 64 × 4 |
| 每帧输出 | 256 bytes | 32 × 8 |
| 输出字节/通道/帧 | 8 bytes | = 64 采样 |
| Snapshot 每 64bit 读 | 64 采样 | Scale = 2^6 |
| 叶子块容量 | 16M 采样 | LeafBlockSamples = 2^24 |
