# AGENTS.md — DSView

DSView is a Qt5/Qt6 C++ GUI application for DreamSourceLab instruments (logic analyzers, oscilloscopes), based on sigrok/PulseView.

## Build (Linux)

```bash
# Configure (only needed once, or after CMakeLists.txt changes):
cmake -B cmake-build-debug-system-gcc13 -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_COMPILER=gcc-13 -DCMAKE_CXX_COMPILER=g++-13

# Build (ninja-based):
cmake --build cmake-build-debug-system-gcc13
```

- C++11, C99 forced via `-std=c++11` / `-std=c99`.
- `-O3` added unconditionally via `add_compile_options(-O3)` — applies even to Debug builds, so `-g` is present but optimization is still -O3.
- Output binary → `build.dir/DSView` (regardless of build dir).
- `compile_commands.json` generated in the ninja build dir; symlink or point LSP at it.
- Tests are disabled by default (`ENABLE_TESTS FALSE`); the `test/` dir is gitignored.

## Run

```bash
./run_dsview.sh          # Sets PYTHONHOME=/usr, then execs build.dir/DSView
```

`PYTHONHOME=/usr` required for Python protocol decoders.

## Project layout

| Dir | Role |
|---|---|
| `DSView/` | Qt C++ application (main.cpp entrypoint) |
| `libsigrok4DSL/` | Hardware drivers (uart-vcd, DSL devices, demo) |
| `libsigrokdecode4DSL/` | Protocol decoder C lib + ~150 Python decoders |
| `common/` | minizip, xlog logger |
| `low_part/UART_V1.0/` | MCU firmware: app_dma.c, gpio_event.c/h, main.c |
| `low_part/bridge/` | Python serial-to-TCP bridge |

## UART_VCD driver — current state (2026-06)

### Architecture
- **TCP-only**: driver connects to the host configured by `SR_CONF_TCP_HOST`
  on port `12345`; the compile-time default is defined in `uart_vcd.h`
- **Device name**: "Uart VCD" (was "FT232R USB UART")
- **Python bridge**: `low_part/bridge/serial_bridge.py` — scans serial ports, user picks one, listens as TCP server, bridges serial↔TCP bidirectionally (thread-based, Windows-compatible)
- **Flow**: MCU → Serial → Python bridge (TCP server on :12345) → DSView (TCP client)

### Protocol v2 (current, documented in `test_uart_vcd_event_protocol_2.md`)
```
[uint24_le delta_ticks:3B] [header:1B] [payload...]
```
- delta_ticks: raw 24MHz systimer ticks (MCU `stimer_get_tick()`), PC maps 1:1 to samples
- Samplerate: 24MHz (matching MCU timer resolution)
- Header: bit7=mode(0=GPIO,1=string), bits6-5=sub, bits4-0=param

**GPIO** (mode=0): sub=low(0)/high(1)/toggle(2), param=channel(0-23), total=4B
**String** (mode=1): sub=hex(0)/ascii(1), param=label_len(0-31)
  Payload: [channel:1B] [total_len:1B] [label:param B] [data:total_len-param B] [pad to 4B]

### PC parser (`ev2_blow_buf`)
- Zero-copy from read buffer (no memcpy when no partial data)
- Events buffered before emitting samples (delta idle → then state change)
- UART TX sim engine renders bytes as 10-bit serial on RX channels (D24-D31)
- Hex mode: each data byte → 2 hex ASCII chars
- ASCII mode: raw bytes
- Per-ch 64B FIFO for UART TX bytes
- `uart_tx_active` flag skips UART processing when idle (big perf win)
- Per-callback limit: 65,536 events → yields to UI
- Per-event delta clamp: 12M samples max
- Emits `LA_SPARSE_EVENTS`; it does not expand idle time into dense samples

### MCU firmware (`low_part/UART_V1.0/`)
- **gpio_event.c**: protocol v2 encoder, only sends high/low (toggle converted locally)
- **app_dma.c**: ping-pong DMA buffer (zero-copy from producer to `uart_send_dma`)
- `stimer_get_tick()` returns raw 24MHz ticks
- `gpio_event_send_string(ch, render_mode, label, label_len, data, data_len)`
- `gpio_event_reset_timer()` to reset tick base

### Key files
| File | Role |
|------|------|
| `libsigrok4DSL/hardware/uart_vcd/uart_vcd.c` | TCP-only driver, protocol v2 parser, UART TX sim |
| `libsigrok4DSL/hardware/uart_vcd/uart_vcd.h` | Driver config, context struct |
| `DSView/res/uart-vcd0.def.dsc` | Default profile for 32 channels and 8 UART decoders; values must stay synchronized with `uart_vcd.h` |
| `low_part/UART_V1.0/gpio_event.c` | MCU protocol v2 encoder |
| `low_part/UART_V1.0/app_dma.c` | MCU DMA + main_loop |
| `low_part/bridge/serial_bridge.py` | Python serial↔TCP bridge |
| `test_uart_vcd_event_protocol_2.md` | Protocol v2 specification |

### Past issues resolved
- Timer wrap-around (32-bit → huge delta): clamped to 1 sample
- UI freeze from large deltas: per-callback sample/event limits
- Hex FIFO overflow: increased FIFO 16→64B, fixed mask `0x3F`
- Snapshot reader stride: kept 64-sample chunks, batched at driver level
- Input buffer overflow: increased to 1MB, drain on overflow
- TCP blocking: set `O_NONBLOCK` on TCP socket after connect
- State drift on restart: MCU sends absolute high/low, never toggle
- GPIO timing: delta emitted BEFORE state change (idle → then toggle)
- Virtual RX UART: 6Mbaud at 24MHz = 4 samples/bit
- Loop sparse prune is throttled, avoiding repeated `vector::erase()` when the
  loop window reaches the max sample count
- Native UART decode is deferred while capture is running; single/loop decode
  runs after stop or acquisition end

### Memory model
- `uart_vcd.c` forwards absolute-time `LA_SPARSE_EVENTS`; it does not expand idle time into dense samples
- `LogicSnapshot` stores UART_VCD as sparse per-channel edges; memory follows
  edge count, not 24MHz sample count
- Dense 32-channel 24MHz storage would consume about 97.5MB/s; UART_VCD avoids
  that path for normal capture
- DSL `SR_CONF_RLE` is an FPGA acquisition feature and does not provide a
  compressed Snapshot representation

## License
GPLv3+
