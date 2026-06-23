# UART_VCD 当前开发状态

更新时间：2026-06

> Current implementation note: UART_VCD now uses 28 GPIO channels (D0-D27)
> plus 4 direct LOG render channels (DEBUG/INFO/WARN/ERROR). The old `0x80`
> label frame and `gpio_event_send_label()` API are deprecated/removed. Direct
> text uses `0xC0/0xE0` frames carrying
> `[level][label_len][data_len][label][data]`. `.dsl` saving stores direct logs
> in `capture.dsl.txt` and loop-mode rendering/export accounts for
> `LogicSnapshot::get_loop_offset()`.

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

底层普通帧为 v3-only 无 magic 事件帧：

```text
[uint24_le delta_ticks][header][payload]
```

- `0xA0` sync 帧用于错包后的重同步，避免 payload 被误判为事件头后长期漂移。
- timer/sample rate：24 MHz。
- D0-D27：GPIO low/high/toggle，MCU toggle 在本地转换为 absolute high/low。
- 4 个 direct LOG render 通道：DEBUG、INFO、WARN、ERROR。PC 通过
  `SR_DF_UART_VCD_TEXT` 直接推入 annotation row，不再合成 8N1，也不运行 UART
  decoder。

```text
v3 gpio:  [delta][header] = 4 bytes
v3 sync:  [delta][0xA0][gpio_state32][inv_gpio_state32][0x55][0xAA][0x5A][0xA5] = 16 bytes
v3 text:  [delta][0xC0/0xE0][level][label_len][data_len][label][data][padding]
```

v3 详细定义见 `test_uart_vcd_event_protocol_v3.md`。

## 关键实现

| 文件 | 作用 |
|---|---|
| `libsigrok4DSL/hardware/uart_vcd/uart_vcd.c` | TCP、v3 parser、sparse logic、direct text |
| `libsigrok4DSL/hardware/uart_vcd/uart_vcd.h` | 驱动配置和 context |
| `low_part/UART_V1.0/gpio_event.c` | MCU v3 encoder 和 ping-pong DMA TX |
| `low_part/UART_V1.0/app_dma.c` | MCU UART/平台初始化和测试主循环 |
| `low_part/bridge/serial_bridge.py` | UART 到 TCP 的单向转发 |
| `test_uart_vcd_event_protocol_v3.md` | v3 direct text 协议定义 |
| `uart_vcd_data_path.md` | 数据通路和内存分析 |

## 当前参数

| 参数 | 值 |
|---|---:|
| MCU/bridge UART | 3,000,000 baud |
| TCP port | 12345 |
| sample rate | 24,000,000 samples/s |
| channel count | 28 GPIO + 4 LOG render traces |
| input buffer | 1 MiB |
| sparse event | 16 bytes：absolute sample + 32-bit state |
| event batch | 4,096 records / 64 KiB |
| per-callback event limit | 65,536 |
| per-event delta clamp | 16,777,215 |

## 当前性能策略

- 驱动向上提交 `LA_SPARSE_EVENTS`，采集阶段不再展开 24 MHz dense sample。
- `LogicSnapshot` 对 UART_VCD 使用 sparse edge backend，内存和边沿数相关。
- loop mode 到达最大 sample 窗口后，只按约 1 秒 sample 步长做 sparse prune，
  避免每个包都 `vector::erase()` 导致 CPU 突增。
- v3 direct text 不进入 native UART decode，annotation 注入 UI 信号按 256 条
  节流刷新，避免高频文本造成 UI 刷新风暴。
- 正常 loop prune 会低频打印：

```text
DSView: LogicSnapshot sparse prune: loop_offset=... elapsed=... ms
```

## 已知约束

- `LogicSnapshot` 的 UART_VCD backend 仅保存真实边沿，内存与边沿数成正比。
- decoder 在相邻边沿之间使用 constant channel，不再展开 24 MHz dense 数据。
- 保存和导出仍会按块临时物化位图，使用后立即释放。
- PC parser 遇到非法帧会打印错包、重新同步并继续采集。
- DSL RLE 是 FPGA 侧能力，不能直接解决 UART_VCD Snapshot 内存。
- UART_VCD 当前按 28 个 GPIO 逻辑通道存储；4 个 LOG 通道是 direct annotation，
  不进入逻辑位图。
- 默认 profile 的 sample rate/decoder baud 可能与驱动常量漂移，修改配置时需
  同时核对 `uart_vcd.h` 和 `DSView/res/uart-vcd0.def.dsc`。
- `send_event_test.py` 是旧 protocol v2 TCP 测试服务器；v3 需要补充新的测试发送器。
- `test_uart_vcd_event_protocol.md` 仅保留旧 varint protocol 的废弃说明。

## 协议注意点

- GPIO event 固定 4 bytes，当前对 24 MHz delta + channel + high/low 来说
  已经比较紧凑，主要瓶颈不在 GPIO event 编码。
- v3 direct text 已去掉 8N1 合成和 UART decoder 重解码开销。
- HEX render 会把每个 data byte 放大为两个 ASCII 字符；能用 ASCII 时优先用
  ASCII，可直接减少 RX 边沿和后续解码工作。
- MCU 当前只保留 `gpio_event_send_text(gpio_event_level_t,
  gpio_event_render_mode_t, label, label_len, data, data_len)` 文本接口。

## 构建

```bash
cmake --build cmake-build-debug-system-gcc13
```

输出：

```text
build.dir/DSView
```
