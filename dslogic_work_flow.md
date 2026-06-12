# DSLogic 硬件驱动工作流程

## 架构概览

```
FPGA 硬件 ──USB bulk EP 0x86──→ libusb transfer buffer ──零拷贝──→ ds_data_forward ──拷贝──→ LogicSnapshot mipmap 树 ──→ UI 渲染
```

USB transfer buffer 预分配 + 循环复用（resubmit），驱动层无数据拷贝。

---

## 1. 采集启动 (`dev_acquisition_start` → `dsl.c`)

**文件**: `libsigrok4DSL/hardware/DSL/dslogic.c:1406-1519`, `dsl.c:1011-1294`

### 1.1 状态初始化
```c
devc->num_samples = 0;
devc->num_bytes = 0;
devc->actual_samples = (limit_samples + 1023) & ~1023;       // 1024 对齐
devc->actual_bytes   = actual_samples / 64 * channels * 8;   // 目标总字节数
```
采样数以 **64 sample 原子块** 为单位（`DSLOGIC_ATOMIC_SAMPLES=64`），每个原子块 8 字节。

### 1.2 FPGA 配置发送 (`dsl_fpga_arm`)
通过 USB EP `0x02` 批量下发 `DSL_setting` 结构体（312 字节），包含：
- 采样率分频器 (`div_l/div_h`)
- 采集计数 (`cnt_l/cnt_h` = `actual_samples >> 4`)
- 触发位置 (`tpos_l/tpos_h`)
- 通道使能位图 (`ch_en_l/ch_en_h`)
- RLE 模式位（`mode` bit 3）
- stream/buffer 模式位
- 16 级串行触发配置

### 1.3 USB Transfer 提交 (`dsl_start_transfers`)
- **1 个 trigger header transfer** → callback: `receive_header`（一次性，收到触发点后释放）
- **N 个 data transfer** → callback: `receive_transfer`（循环 resubmit）

Transfer 数量：
- Stream 模式：`total_buffer_time / buffer_time`（上限 64 个并行）
- Buffer 模式：Linux 下 1 个，Windows 下 4-16 个

每个 transfer buffer 大小：
- Stream 模式：10ms（USB3）或 20ms（USB2）的数据量
- Buffer 模式：1MB

---

## 2. 数据接收（零拷贝机制）(`receive_transfer` → `dsl.c:2304`)

```c
static void receive_transfer(struct libusb_transfer *transfer) {
    uint8_t *cur_buf = transfer->buffer;  // USB 驱动层分配的缓冲区
    ...
    // LOGIC 模式：直接挂指针，不做拷贝
    packet.type = SR_DF_LOGIC;
    logic.format = LA_CROSS_DATA;        // 交叉位图格式
    logic.length = transfer->actual_length;
    logic.data = cur_buf;                // ← 零拷贝：USB buffer 指针直接传

    ds_data_forward(sdi, &packet);       // → 管线

    resubmit_transfer(transfer);         // buffer 复用，下一轮 USB 接收
}
```

**零拷贝原理**：
1. libusb 分配 transfer buffer → FPGA 把数据 DMA 写入这个 buffer
2. 驱动拿到 buffer 后不拷贝，直接把指针填入 `sr_datafeed_logic.data`
3. `ds_data_forward` 把 packet 传给 UI 管线
4. UI 管线（`LogicSnapshot`）把数据**拷贝**进 mipmap 树
5. 拷贝完成后 USB buffer 可以安全复用

**Buffer 生命周期**：
```
malloc → libusb_submit → [FPGA DMA 写入] → receive_transfer → ds_data_forward(指针) → libusb_submit(复用) → ...
                                                                                                ↓
                                                                               [stop/error] → free
```

---

## 3. RLE 压缩（FPGA 硬件实现）

**配置**: `DSL_setting.mode` bit 3 = `RLE_MODE_BIT`
**位置**: `dsl.c:1063`

RLE 压缩在 **FPGA 硬件内部**完成：
- 使能时，FPGA 内部 buffer 存储 (值, 重复次数) 而非原始采样点
- USB 输出到驱动时已**解压为正常 LA_CROSS_DATA 格式**
- 驱动完全透明，不感知 RLE
- RLE 的作用是增大 FPGA 内部 buffer 能容纳的等效采样数

**效果**: 16ch@20MHz 静默信号，硬件 buffer 可存数秒数据；非 RLE 只能存 ~100ms。

---

## 4. 采集停止 (`dev_acquisition_stop`)

**文件**: `dsl.c:2003-2041`

两阶段停止：

**阶段 1 — 中止信号**：
```c
devc->abort = TRUE;
dsl_wr_reg(sdi, CTR0_ADDR, bmFORCE_RDY);  // 强制 FPGA 就绪，立即 dump buffer
```

**阶段 2 — 清理**：
- `receive_transfer` 检测到 `abort` → `status = DSL_STOP`
- 不再 `resubmit`，改为 `free_transfer(transfer)`
- 所有 transfer 释放完毕 → `finish_acquisition()` → 发送 `SR_DF_END`

---

## 5. 与 UART_VCD 的关键对比

| | DSLogic | UART_VCD |
|---|---|---|
| 数据来源 | FPGA USB bulk | TCP socket |
| 硬件压缩 | FPGA RLE（增大等效 buffer 深度） | 无 |
| 驱动数据格式 | LA_CROSS_DATA（直接转发） | 事件 → 展开为 LA_CROSS_DATA |
| 驱动拷贝 | 零拷贝（USB buffer 指针） | batch_buf 构建 + memcpy |
| Transfer 复用 | resubmit（同一 buffer 循环用） | batch_buf 循环覆写 |
| 空闲采样存储 | 硬件 RLE 压缩后不占 USB 带宽 | 每个空闲 tick 展开为 1bit 存入 |
| 内存增速 | RLE + 固定 total_samples 限流 | 24MHz×32ch = 96MB/s 无上限（loop 模式） |

**核心差异**：DSLogic 的 RLE 压缩使静默期不产生 USB 数据，驱动只转发状态变化区间的少量字节。UART_VCD 的每个 event delta 必须逐采样点展开成 96MB/s 的位图。

---

## 6. 关键常量和结构体

| 常量 | 值 | 含义 |
|---|---|---|
| `DSLOGIC_ATOMIC_SAMPLES` | 64 | 原子采样数 |
| `DSLOGIC_ATOMIC_SIZE` | 8 | 原子块字节数 |
| `SAMPLES_ALIGN` | 1023 | 采样数对齐掩码 |
| `NUM_SIMUL_TRANSFERS` | 64 | 最大并行 transfer 数 |
| `MAX_EMPTY_POLL` | 16 | 空闲检测阈值 |

**USB 端点**：
- EP `0x02` OUT：FPGA 配置 + 位流
- EP `0x86` IN：采集数据 + 触发头

**`DSL_setting` 结构体** (`dsl.h:1245-1282`)：312 字节，包含 mode、divider、counter、trigger position、通道使能、16 级触发配置。

---

## 7. 完整采集时序

```
dev_acquisition_start
 ├─ 计算 actual_samples/bytes
 ├─ 配置 probe 触发参数
 ├─ DSL_CTL_STOP（停止上次 GPIF）
 ├─ dsl_fpga_arm（EP 0x02 下发 FPGA 配置）
 ├─ dsl_start_transfers（EP 0x86 提交 USB transfer）
 ├─ 注册 libusb pollfd → receive_data 回调
 ├─ DSL_CTL_START（启动 FPGA 采集）
 └─ 发送 DF_HEADER

receive_data（glib 事件循环回调）
 └─ libusb_handle_events
     ├─ receive_header → 收到触发点 → DF_TRIGGER → free
     └─ receive_transfer → 收到数据块
          ├─ packet.type = SR_DF_LOGIC, data = cur_buf（零拷贝）
          ├─ ds_data_forward(sdi, &packet) → UI 管线 → LogicSnapshot
          ├─ resubmit_transfer（buffer 复用）或 free_transfer（停止）
          └─ num_bytes >= actual_bytes → DSL_STOP

dev_acquisition_stop
 ├─ bmFORCE_RDY → FPGA dump → DSL_STOP
 └─ DSL_CTL_STOP → 清理

finish_acquisition
 ├─ 发送 DF_END
 └─ 释放 transfers 数组
```
