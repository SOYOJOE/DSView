# UART_VCD 驱动开发会话记忆

## 项目概述
为 DSView（基于 libsigrok4DSL）添加一个新的硬件驱动 `uart-vcd`，通过 FT232R USB UART 串口接收 32bit 数据，将每个 bit 映射为一个逻辑分析通道（共 32 通道 D0-D31），在界面上显示波形翻转。

---

## 文件清单

| 文件 | 说明 |
|------|------|
| `libsigrok4DSL/hardware/uart_vcd/uart_vcd.h` | 驱动头文件，常量定义，上下文结构体 |
| `libsigrok4DSL/hardware/uart_vcd/uart_vcd.c` | 驱动主实现（632行） |
| `libsigrok4DSL/hwdriver.c` | 添加 uart_vcd 驱动注册 |
| `libsigrok4DSL/config.h` | 添加 `#define HAVE_UART_VCD 1` |
| `CMakeLists.txt` | 将 uart_vcd.c 加入编译源列表 |
| `DSView/pv/config/appconfig.cpp` | 已清理调试日志 |
| `uart_vcd_data_path.md` | 数据通路说明文档 |

---

## 关键设计决策

### 1. 串口配置
- 打开标志: `O_RDWR | O_NOCTTY`（无 O_NONBLOCK）
- 读取模式: `VMIN=0, VTIME=1`（最多阻塞 100ms）
- 事件轮询: `sr_session_source_add(fd, G_IO_IN, 100ms timeout)`

### 2. 32bit 数据格式
- 每个采样 = 连续 4 个串口字节，**小端序**
- `sample = byte0 | (byte1<<8) | (byte2<<16) | (byte3<<24)`
- bit[0] → D0, bit[1] → D1, ..., bit[31] → D31
- bit=1 显示高电平，bit=0 显示低电平

### 3. 位打包 (pack_output_block)
- 每 64 个采样（256 字节串口输入）打包为一个输出帧
- 输出帧: 256 字节 = 32 通道 × 8 字节/通道
- 每字节 = 8 个连续时间采样（同通道），LSB 较早
- 格式: LA_CROSS_DATA, unitsize=1

### 4. 设备类型
- `dev_type = DEV_TYPE_USB`（而非 DEV_TYPE_SERIAL）
- 原因: DSView `is_hardware()` 仅检查 `DEV_TYPE_USB`
- 使 Loop 模式、stream mode 等硬件特性可用

### 5. Loop 模式
- 启用条件: `SR_CONF_OPERATION_MODE = LO_OP_STREAM` + `SR_CONF_LOOP_MODE`
- Loop 模式下不自动停止，持续转发数据，由用户手动停止

### 6. SR_DF_END 发送策略
- **由 receive_data 回调发送**，而非 hw_dev_acquisition_stop
- 原因: session 循环会两次调用 stop，导致双重 END 引发 crash
- 回调检测 `!collecting` 时发送 END 并返回 FALSE

---

## 已修复的问题

1. **"Unknown capability" 日志刷屏** — config_get/set 默认分支静默返回 SR_ERR_NA，添加常用 key 处理
2. **停止采集后页面死机** — 移除 O_NONBLOCK，添加 100ms poll 超时
3. **第二次采集无数据** — hw_dev_acquisition_start 添加串口重连检查，stop 不再关闭 fd
4. **重复设备条目** — hw_scan 通过 drvc->instances 去重
5. **Loop 模式不可用** — dev_type 改为 DEV_TYPE_USB，OPERATION_MODE 返回 LO_OP_STREAM
6. **Loop 模式停止崩溃** — SR_DF_END 改为由回调统一发送

---

## 当前配置参数

| 参数 | 默认值 | 可修改 |
|------|--------|--------|
| 串口路径 | /dev/ttyUSB0 | 仅代码修改 |
| 波特率 | 115200 | config_set |
| 采样率 | 100 kHz | config_set |
| 采样数上限 | 100K | config_set |
| 通道数 | 32 | 固定 |
| 模式 | Logic Analyzer | 固定 |
| Loop | false | 界面切换 |

---

## 编译命令

```bash
cmake --build /home/soyo/Telink/workspace/DSView/cmake-build-debug-system-gcc13
```

输出: `/home/soyo/Telink/workspace/DSView/build.dir/DSView`
