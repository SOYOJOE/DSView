# UART_VCD Event + Delta Time 协议测试数据

## 协议说明

### 数据包格式
```
[varint delta_time] [varint toggle_mask] ... [varint delta_time] [varint toggle_mask] [0x00]
```

- `delta_time`: 距上一事件的 tick 数 (varint, >0)
- `toggle_mask`: 发生翻转的 GPIO mask (varint, 32bit)
- `delta_time=0`: 流结束标记

### Varint 编码规则 (LE)
每字节低 7 位为数据，bit7=1 表示还有后续字节:
```
while value > 0x7F: emit (value & 0x7F) | 0x80; value >>= 7
emit value & 0x7F
```

### 时序约定
- Tick 周期: **1us**
- 采样率自动设为 **1MHz**
- 首个事件前 GPIO 状态 = 0x00000000
- 事件处理：先输出 delta_time 个旧状态的采样，再 XOR toggle_mask 翻转 GPIO

### 解码恢复逻辑
```
gpio_state = 0
for each event:
    for i in 1..delta_time: output_sample(gpio_state)
    gpio_state ^= toggle_mask
```

---

## 测试场景 1：基础 GPIO 翻转 (3 路 GPIO)

### 场景描述
| 时间(us) | 事件 | GPIO变化 |
|----------|------|----------|
| 0        | 初始 | all=0    |
| 100      | GPIO3 上升 | D3=1 |
| 200      | GPIO3 下降 | D3=0 |
| 300      | GPIO7 上升 | D7=1 |
| 350      | GPIO0 上升 | D0=1 |
| 400      | GPIO7,GPIO0 下降 | D7=0,D0=0 |
| 500      | GPIO3 上升 | D3=1 |
| 600      | GPIO3 下降 | D3=0 |
| -        | 结束 | - |

### 事件编码计算

| # | delta | mask      | varint(delta) | varint(mask) | 字节总长 |
|---|-------|-----------|---------------|--------------|----------|
| 1 | 100   | 0x00000008| 64            | 08           | 2        |
| 2 | 100   | 0x00000008| 64            | 08           | 2        |
| 3 | 100   | 0x00000080| 64            | 80 01        | 3        |
| 4 | 50    | 0x00000001| 32            | 01           | 2        |
| 5 | 50    | 0x00000081| 32            | 81 01        | 3        |
| 6 | 100   | 0x00000008| 64            | 08           | 2        |
| 7 | 100   | 0x00000008| 64            | 08           | 2        |
| E | 0     | -         | 00            | -            | 1        |

### 完整 Hex Stream (17 bytes)
```
64 08 64 08 64 80 01 32 01 32 81 01 64 08 64 08 00
```

### 预期 DSView 波形 (— 高电平, _ 低电平)

```
Time(us):  0                 100             200         300 350 400         500             600
D0:        _______________________________                        ----________________________________
D3:        _____--------_______________________________                                    ----_______
D7:        ___________________----________----------------------------------------------------------------
```

对应事件:
| 时间(us) | GPIO 状态变化 | D0 | D3 | D7 |
|----------|-------------|----|----|-----|
| 0-100    | 初始, 全低  | 0  | 0  | 0  |
| 100      | D3↑        | 0  | 1  | 0  |
| 200      | D3↓        | 0  | 0  | 0  |
| 300      | D7↑        | 0  | 0  | 1  |
| 350      | D0↑        | 1  | 0  | 1  |
| 400      | D7↓, D0↓   | 0  | 0  | 0  |
| 500      | D3↑        | 0  | 1  | 0  |
| 600      | D3↓        | 0  | 0  | 0  |

---

## 测试场景 2: GPIO31 高位翻转 (测试大 mask varint)

### 场景描述
| 时间(us) | 事件 | GPIO变化 |
|----------|------|----------|
| 0        | 初始 | all=0    |
| 500      | GPIO31 上升 | D31=1 |
| 1000     | GPIO31 下降 | D31=0 |
| -        | 结束 | - |

### 事件编码计算

| # | delta | mask        | varint(delta) | varint(mask)     | 字节总长 |
|---|-------|-------------|---------------|------------------|----------|
| 1 | 500   | 0x80000000  | F4 03         | 80 80 80 80 08   | 7        |
| 2 | 500   | 0x80000000  | F4 03         | 80 80 80 80 08   | 7        |
| E | 0     | -           | 00            | -                | 1        |

Varint(500): 500 = 0b1_1111_0100 → byte0=0xF4, byte1=0x03
Varint(0x80000000): 2^31 → needs 5 bytes: 80 80 80 80 08

### 完整 Hex Stream (15 bytes)
```
F4 03 80 80 80 80 08 F4 03 80 80 80 80 08 00
```

---

## 测试场景 3: 密集翻转 (压力测试)

### 场景描述
GPIO0 和 GPIO1 交替翻转，每 10us 一次变化，共 10 个事件。

### 编码
| #  | delta | mask        | bytes |
|----|-------|-------------|-------|
| 1  | 10    | 0x00000001  | 0A 01 |
| 2  | 10    | 0x00000002  | 0A 02 |
| 3  | 10    | 0x00000001  | 0A 01 |
| 4  | 10    | 0x00000002  | 0A 02 |
| 5  | 10    | 0x00000001  | 0A 01 |
| 6  | 10    | 0x00000002  | 0A 02 |
| 7  | 10    | 0x00000001  | 0A 01 |
| 8  | 10    | 0x00000002  | 0A 02 |
| 9  | 10    | 0x00000001  | 0A 01 |
| 10 | 10    | 0x00000002  | 0A 02 |
| E  | 0     | -           | 00    |

### 完整 Hex Stream (21 bytes)
```
0A 01 0A 02 0A 01 0A 02 0A 01 0A 02 0A 01 0A 02 0A 01 0A 02 00
```

### 预期波形 (— 高电平, _ 低电平)

```
Time(us):  0  10 20 30 40 50 60 70 80 90 100
D0:        _  -  -  _  _  -  -  _  _  -  _
D1:        _  _  -  -  _  _  -  -  _  _  -
```

| 时间(us) | D0 | D1 | 说明 |
|----------|----|-----|------|
| 0-9      | 0  | 0  | 初始 |
| 10-19    | 1  | 0  | D0↑ |
| 20-29    | 1  | 1  | D1↑ |
| 30-39    | 0  | 1  | D0↓ |
| 40-49    | 0  | 0  | D1↓ |
| 50-59    | 1  | 0  | D0↑ |
| 60-69    | 1  | 1  | D1↑ |
| 70-79    | 0  | 1  | D0↓ |
| 80-89    | 0  | 0  | D1↓ |
| 90-99    | 1  | 0  | D0↑ |

---

## 测试场景 4: 长空闲 (测试 delta varint 多字节)

### 场景描述
GPIO5 初始翻转后空闲 100,000us (100ms)，再翻转。

| #  | delta    | mask        | varint(delta)      | varint(mask) |
|----|----------|-------------|---------------------|--------------|
| 1  | 100      | 0x00000020  | 64                  | 20           |
| 2  | 100000   | 0x00000020  | C0 8D 06            | 20           |
| E  | 0        | -           | 00                  | -            |

Varint(100000): 100000 = 0x186A0
  byte0 = 0xA0 | 0x80 = 0xA0  (remainder: 100000 & 0x7F = 0x20=32, but wait)
  
  Let me compute properly:
  100000 = 0b1_1000_0110_1010_0000
  Group into 7 bits from LSB: 010_0000 | 000_1101 | 000_0110 | 1
  byte0: 0b1010_0000 = 0xA0
  byte1: 0b1000_1101 = 0x8D
  byte2: 0b1000_0110 = 0x86
  byte3: 0b0000_0001 = 0x01
  
  Wait, that's 4 bytes. Let me reconsider.
  100000 = 0x186A0
  In binary: 0001 1000 0110 1010 0000
  Groups of 7 (LSB first):
    bits 0-6:   010 0000 → 0x20 | 0x80 = 0xA0
    bits 7-13:  000 1101 → 0x0D | 0x80 = 0x8D
    bits 14-20: 000 0110 → 0x06 | 0x80 = 0x86
    bits 21-27: 000 0001 → 0x01 (MSB=0, last)
  
  So varint(100000) = A0 8D 86 01 — 4 bytes

  Hmm wait, let me verify:
  Decode: 0xA0 → value |= 0x20 << 0 = 0x20, shift=7
          0x8D → value |= 0x0D << 7 = 0x680, shift=14, total=0x6A0
          0x86 → value |= 0x06 << 14 = 0x18000, shift=21, total=0x186A0
          0x01 → value |= 0x01 << 21 = 0x200000? No, 0x01 << 21 is not right.
          
  Let me recompute. 100000 in decimal.
  100000 / 128 = 781 remainder 32
  781 / 128 = 6 remainder 13
  6 / 128 = 0 remainder 6
  
  So bytes: 32|0x80=0xA0, 13|0x80=0x8D, 6=0x06
  
  Let me verify: 32 + 13*128 + 6*128*128 = 32 + 1664 + 98304 = 100000 ✓
  
  So varint(100000) = A0 8D 06 — 3 bytes, not 4. I was wrong with the 4-byte encoding.

OK great, 3 bytes.

### 完整 Hex Stream (7 bytes)
```
64 20 A0 8D 06 20 00
```

---

## 串口模拟发送方法

### Linux 环境
```bash
# 方法1: 用 echo/printf 发送原始 hex
printf '\x64\x08\x64\x08\x64\x80\x01\x32\x01\x32\x81\x01\x64\x08\x64\x08\x00' > /dev/ttyUSB0

# 方法2: 虚拟串口对测试 (socat)
socat -d -d PTY,raw,echo=0,link=/tmp/vcom0 PTY,raw,echo=0,link=/tmp/vcom1 &
# 修改 UART_VCD_DEFAULT_SERIAL_PORT 为 /tmp/vcom0
# 构建后运行 DSView
printf '\x64\x08\x64\x08...' > /tmp/vcom1

# 方法3: Python 脚本发送
python3 -c "
import serial
ser = serial.Serial('/dev/ttyUSB0', 1000000)
ser.write(bytes([0x64,0x08,0x64,0x08,0x64,0x80,0x01,0x32,0x01,0x32,0x81,0x01,0x64,0x08,0x64,0x08,0x00]))
ser.close()
"
```

### 启用 Event 协议
修改 `libsigrok4DSL/hardware/uart_vcd/uart_vcd.h`:
```c
#define UART_VCD_DEFAULT_PROTOCOL UART_VCD_PROTOCOL_EVENT
```

然后重新编译。

---

## 预期输出验证要点

| 验证项 | 场景1预期 |
|--------|-----------|
| D0 首次上升 | t=350us |
| D0 首次下降 | t=400us |
| D3 首次上升 | t=100us |
| D7 首次上升 | t=300us |
| 总采样数 | 600 samples |
| 输出块数(64samples/block) | 10 blocks (最后一块不足64被填充0) |
| 结束包 | 收到 delta_time=0 后发 SR_DF_END |

---

## 带宽对比

| 模式 | 场景1数据量 | 说明 |
|------|------------|------|
| 原始模式 (32bit/sample@100kHz) | 600×4 = 2400 bytes | 固定采样 |
| Event 模式 | **17 bytes** | 仅变化事件 |

**压缩比**: 2400/17 ≈ **141x**

注意: 原始模式 100kHz 下 600us 只有 60 个采样，这里按 1MHz 对齐对比。实际原始模式如果用 1MHz 会是 600×4=2400 bytes。
