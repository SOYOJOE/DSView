# UART_VCD Event Protocol v2

## 概述

- 24 路 GPIO 通道 (D0-D23) + 8 路字符串通道 (D24-D31)
- GPIO 支持高/低/反转三种操作
- 字符串通道支持 hex 和 ASCII 两种渲染模式
- 时间戳使用 24MHz systimer 计时，3 字节 LE 编码 delta-tick

## 数据包格式

```
[uint24_le delta_ticks] [data_block...]
```

- `delta_ticks`: 距上一事件的 systimer tick 数 (24MHz, 1tick = 1/24µs), 3 字节小端序
- `data_block`: (1 + 4n) 字节，1 字节头 + payload，4 字节对齐填充

```
字节布局:
  byte 0:      header  (1B)
  byte 1..N:   payload (N bytes, N = ceil(payload_len/4) × 4)
```

总数据段长度 = 1 + 4n (n = 0,1,2,...), 不足 4 字节对齐时补 0x00.

## Header 字节

```
Bit 7:        mode
  0 = GPIO 电平控制
  1 = 字符串发送

Bits 6-5:     sub-mode (含义由 mode 决定)
Bits 4-0:     param (含义由 mode 决定)
```

### GPIO 模式 (bit7=0)

| sub-mode (bits6-5) | 操作 |
|---|---|
| 00 | low — 拉低 |
| 01 | high — 拉高 |
| 10 | toggle — 反转 |
| 11 | reserved |

param (bits4-0): GPIO 通道号 (0-23).

> 例: header=0x58 (0b01011000) → mode=0, sub=10(toggle), channel=24

数据段只有 header 字节本身 (n=0, 1+4×0=1B).

### 字符串模式 (bit7=1)

| sub-mode (bits6-5) | 渲染方式 |
|---|---|
| 00 | hex — 数据字节按十六进制渲染 |
| 01 | ascii — 数据字节按 ASCII 字符渲染 |
| 10-11 | reserved |

param (bits4-0): 标签长度 (0-31)。0 表示无标签。payload 总长由 payload 首字节 `total_len` 指定。

## 字符串 Payload 格式

Payload 由 **总长度** + **标签内容** + **数据内容** 三部分紧密排列：

```
┌────────────┬────────────────────┬──────────────────────────┐
│  total_len │  label             │  data                    │
│  uint8 (1B)│  ASCII × param     │  (total_len - param)     │
└────────────┴────────────────────┴──────────────────────────┘
 payload 在线路上的总字节数 = 1 + total_len
```

| 字段 | 字节数 | 说明 |
|---|---|---|
| `total_len` | 1 | label + data 的总字节数 (0–255)，不含自身 |
| `label` | `param` | 标签文本，按 ASCII 原样显示。`param=0` 表示无标签 |
| `data` | `total_len - param` | 数据内容，按 sub-mode 指定的渲染方式显示 |

**计算关系：**

```
total_len  = param + data_len
data_len   = total_len - param
```

| 来源 | 字段 | 范围 | 含义 |
|---|---|---|---|
| header bits4-0 | `param` | 0–31 | 标签长度 |
| payload 首字节 | `total_len` | 0–255 | label + data 的总字节数 |
| 计算得出 | `data_len` | 0–255 | 数据内容的字节数 |

**示例：**

| param | total_len | label | data_len | 说明 |
|---|---|---|---|---|
| 4 | 8 | `"ch0:"` (4B) | 4 | param=标签长, total_len=label+data 的总长 |
| 0 | 5 | (空) | 5 | param=0 无标签, 全部 5B 是数据 |
| 4 | 4 | `"LOG:"` (4B) | 0 | 仅标签，无数据 |
| 2 | 2 | `"> "` (2B) | 0 | 短标签，无数据 |

## 渲染规则

### hex 模式 (sub=00)

标签段按 ASCII 原文显示，数据段每字节转为 2 位十六进制字符，连续显示。

> 例: param=3, label="CH:", data={0x12, 0x34, 0xAB}
> → 渲染结果: **"CH:1234AB"**

### ascii 模式 (sub=01)

所有字节 (含标签和数据) 均按 ASCII 字符直接显示。

> 例: param=3, label="TX:", data={'O','K',0x0D,0x0A}
> → 渲染结果: **"TX:OK\r\n"**

## 完整示例

### 例1: GPIO 反转 D5

```
delta_ticks = 1000 (0xE8, 0x03, 0x00) → 3B LE

header = 0b0_10_00101 = 0x45
  mode=0(GPIO), sub=10(toggle), channel=5

完整包: E8 03 00  45
         └─delta──┘ └header(1+4×0=1B)
```

### 例2: 字符串 HEX 渲染 (RX0)

```
delta_ticks = 50000 (0x50, 0xC3, 0x00)

label="ch0:", param=4 (标签长)
data={0x12, 0x34, 0x56, 0x78}, data_len=4
total_len = 4(label) + 4(data) = 8

header = 0b1_00_00100 = 0x84
  mode=1(string), sub=00(hex), param=4

data_block = 84  08  63 68 30 3A  12 34 56 78  00 00 00
             H   T   c  h  0  :   1  2  3  4   pad(3B)
             1B  ──payload(1+8=9B)─────────── ──对齐──
data_block 总长 = 1 + ceil(9/4)×4 = 1 + 12 = 13B

完整包: 50 C3 00  84 08 63 68 30 3A 12 34 56 78 00 00 00
         └delta─┘ └───────────data_block(13B)────────────┘

PC 渲染: "ch0:12345678"
```

### 例3: 字符串 ASCII 渲染 (RX1)

```
delta_ticks = 100 (0x64, 0x00, 0x00)

label="LOG:", param=4 (标签长), 无数据段(data_len=0)
total_len = 4(label) + 0(data) = 4

header = 0b1_01_00100 = 0xA4
  mode=1(string), sub=01(ascii), param=4

data_block = A4  04  4C 4F 47 3A  00 00 00
             H   T   L  O  G  :   pad(2B)
             1B  ─payload(1+4=5B)─ ──对齐──

完整包: 64 00 00  A4 04 4C 4F 47 3A 00 00 00
         └delta┘ └───────data_block(9B)──────┘

PC 渲染: "LOG:"
```
