# UART_VCD Event Protocol v2

## 1. Overview

- 24 GPIO channels: D0-D23.
- 8 string/UART channels: RX0-RX7.
- Timestamp clock: 24 MHz.
- All events carry a 24-bit little-endian delta tick.

```text
[delta_ticks:3][header:1][optional payload]
```

Delta is the elapsed MCU `stimer_get_tick()` count since the previous event.
The PC maps one tick to one 24 MHz sample.

## 2. Header

```text
bit 7      mode
bits 6-5   sub-mode
bits 4-0   parameter
```

### GPIO mode

`bit7 = 0`

| Sub-mode | Header bits 6-5 | Operation |
|---|---:|---|
| low | 00 | Set channel low |
| high | 01 | Set channel high |
| toggle | 10 | Toggle channel |
| reserved | 11 | Reserved |

Bits 4-0 are the GPIO channel, valid from 0 to 23. A GPIO event has no
payload and is always 4 bytes total.

Example, toggle D5 after 1000 ticks:

```text
E8 03 00 45
```

The MCU API normally converts `gpio_event_toggle()` to an absolute low/high
event before transmission.

### String mode

`bit7 = 1`

| Sub-mode | Header bits 6-5 | Rendering |
|---|---:|---|
| hex | 00 | Each data byte becomes two uppercase hex ASCII characters |
| ASCII | 01 | Data bytes are sent unchanged |
| reserved | 10-11 | Reserved |

Bits 4-0 contain `label_len`, valid from 0 to 31.

## 3. String payload

The payload is:

```text
[channel:1][total_len:1][label:label_len][data:data_len][zero padding]
```

| Field | Range | Meaning |
|---|---:|---|
| channel | 0-7 | RX0-RX7 |
| total_len | 0-255 | `label_len + data_len` |
| label | 0-31 bytes | ASCII prefix |
| data | remaining bytes | HEX or ASCII rendered data |

Relations:

```text
data_len = total_len - label_len
payload_len = 2 + total_len
payload_padded = align_up(payload_len, 4)
event_len = 4 + payload_padded
```

Padding starts after data and is not rendered.

## 4. Rendering

The PC turns each rendered byte into an 8N1 waveform:

```text
start(0), data bit 0..7, stop(1)
```

Current settings are 24 MHz sample rate and 6 Mbaud, or 4 samples per bit.

HEX example:

```text
label = "ch0:"
data  = 12 34 AB
rendered bytes = "ch0:1234AB"
```

ASCII example:

```text
label = "TX:"
data  = 4F 4B 0D 0A
rendered bytes = "TX:OK\r\n"
```

## 5. Complete examples

### HEX event on RX0

Input:

```text
delta_ticks = 50000
channel = 0
label = "ch0:"
data = 12 34 56 78
```

Calculated fields:

```text
delta = 50 C3 00
header = 84
channel = 00
total_len = 08
payload_len = 10
payload_padded = 12
```

Complete 16-byte event:

```text
50 C3 00 84 00 08 63 68 30 3A 12 34 56 78 00 00
```

Rendered text:

```text
ch0:12345678
```

### ASCII event on RX1

Input:

```text
delta_ticks = 100
channel = 1
label = "LOG:"
data_len = 0
```

Complete 12-byte event:

```text
64 00 00 A4 01 04 4C 4F 47 3A 00 00
```

Rendered text:

```text
LOG:
```

## 6. Validation rules

- Reject or ignore GPIO channels above 23.
- String channel is interpreted as `channel & 7` by the current PC parser.
- If `total_len < label_len`, current PC code treats `data_len` as zero.
- The uint24 delta wraps naturally on the MCU; the PC clamps abnormal deltas.
- No end-of-stream marker exists. Acquisition ends by sample limit or user stop.
