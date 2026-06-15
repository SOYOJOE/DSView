# UART_VCD 当前开发状态

更新时间：2026-06

## 架构

```text
MCU -> UART 3 Mbps -> serial_bridge.py -> TCP :12345
    -> uart-vcd driver -> LogicSnapshot -> DSView UI
```

- 驱动为 TCP client，不直接打开串口。
- bridge 为 TCP server；TCP 未连接时丢弃串口数据。
- 设备名为 `Uart VCD`，驱动名为 `uart-vcd`。
- 设备使用 `DEV_TYPE_USB` 以接入 DSView hardware/stream/loop 流程。

## 当前协议

只使用 protocol v2：

```text
[uint24_le delta_ticks][header][payload]
```

- timer/sample rate：24 MHz。
- D0-D23：GPIO low/high/toggle。
- RX0-RX7：字符串事件，由 PC 合成 8N1 UART 波形。
- 字符串 payload：

```text
[channel][total_len][label][data][padding-to-4-bytes]
```

MCU 的 `gpio_event_toggle()` 在本地状态上转换为 absolute low/high，避免 PC
重连后因丢包产生 toggle 状态漂移。

## 关键实现

| 文件 | 作用 |
|---|---|
| `libsigrok4DSL/hardware/uart_vcd/uart_vcd.c` | TCP、v2 parser、dense sample 展开、UART 波形合成 |
| `libsigrok4DSL/hardware/uart_vcd/uart_vcd.h` | 驱动配置和 context |
| `low_part/UART_V1.0/gpio_event.c` | MCU v2 encoder 和 ping-pong DMA TX |
| `low_part/UART_V1.0/app_dma.c` | MCU UART/平台初始化和测试主循环 |
| `low_part/bridge/serial_bridge.py` | UART 到 TCP 的单向转发 |
| `test_uart_vcd_event_protocol_2.md` | 当前协议定义 |
| `uart_vcd_data_path.md` | 数据通路和内存分析 |

## 当前参数

| 参数 | 值 |
|---|---:|
| MCU/bridge UART | 3,000,000 baud |
| TCP port | 12345 |
| sample rate | 24,000,000 samples/s |
| virtual RX baud | 6,000,000 baud |
| channel count | 32 |
| input buffer | 1 MiB |
| output chunk | 64 samples / 256 bytes |
| batch output | 256 chunks / 64 KiB |
| per-callback event limit | 2,048 |
| per-callback sample limit | 3,145,728 |
| per-event delta clamp | 12,000,000 |

## 已知约束

- `LogicSnapshot` 保存 dense 位图，32 通道 24 MHz 时约增长 97.5 MB/s。
- DSL RLE 是 FPGA 侧能力，不能直接解决 UART_VCD Snapshot 内存。
- UART_VCD 当前忽略 channel disable，默认按全部 32 通道存储。
- 默认 profile 的 sample rate/decoder baud 可能与驱动常量漂移，修改配置时需
  同时核对 `uart_vcd.h` 和 `DSView/res/uart-vcd0.def.dsc`。
- `send_event_test.py` 是当前 protocol v2 TCP 测试服务器。
- `test_uart_vcd_event_protocol.md` 仅保留旧 varint protocol 的废弃说明。

## 构建

```bash
cmake --build cmake-build-debug-system-gcc13
```

输出：

```text
build.dir/DSView
```
