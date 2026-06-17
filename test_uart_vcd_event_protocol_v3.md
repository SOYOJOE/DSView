# UART_VCD Event Protocol v3 Extended

## 1. Goal

v3 extended keeps the low-cost GPIO event path from v2, and replaces RX0-RX3
virtual 8N1 waveform synthesis with direct log annotations. The implementation
is intentionally small:

- mode 0: single GPIO high/low event
- mode 2: sync/resync absolute GPIO state
- mode 3: direct log annotation

Multi-GPIO mask batching remains a future extension and must use a different
header or a versioned sync payload if it is added later.

Normal event frames do not carry a per-frame magic word. Bad-packet recovery is
provided by a low-rate sync frame, so GPIO/text hot paths keep the small v2
wire size while the PC can still recover to a known boundary after corruption.

```text
[uint24_le delta_ticks:3][header:1][payload...]
```

`delta_ticks` is the elapsed 24 MHz MCU systimer tick count since the previous
event that advances sample time. The PC maps one tick to one sample.

## 2. Header Map

Only these header values are valid in v3:

| Header | Mode | Meaning |
|---:|---|---|
| `0x00-0x1B` | mode 0 | GPIO low, channel 0-27 |
| `0x20-0x3B` | mode 0 | GPIO high, channel 0-27 |
| `0xA0` | mode 2 | sync/resync, absolute GPIO state |
| `0xC0` | mode 3 | direct log, HEX render for data |
| `0xE0` | mode 3 | direct log, ASCII render for data |

All other headers are invalid and must be rejected by the PC parser. In
particular, v2 string headers such as `0x86` are not valid v3 frames.

## 3. Mode 0: GPIO Single

```text
[delta:3][header:1]
header = (sub << 5) | channel
sub = 0 low, 1 high
channel = 0..27
```

Rules:

- Frame size is always 4 bytes.
- MCU APIs may keep `toggle`, but it must be converted locally to absolute
  high/low before transmission.
- Delta is emitted before the state change: idle time first, then new level.
- This mode is safe for ISR printing because it preserves every edge.

## 4. Mode 1: Deprecated

Header `0x80` label registration is deprecated and invalid in v3 extended.
Trace names are configured by the DSView profile as log levels.

## 5. Mode 3: Direct Log Annotation

```text
[delta:3][header:1][level:1][label_len:1][data_len:1]
[label:label_len][data:data_len][pad_to_4B]
header = 0xC0 for HEX render of data
header = 0xE0 for ASCII render of data
```

Rules:

- `level` is 0..3 for DEBUG, INFO, WARN, ERROR.
- `label_len` is 0..255. Label bytes are always treated as string bytes.
- `data_len` is 0..255.
- `label_len` and `data_len` may not both be zero.
- Payload length is `3 + label_len + data_len`, padded to 4 bytes with zero
  bytes.
- Frame size is `4 + align4(3 + label_len + data_len)`, and must not exceed
  264 bytes.
- Padding bytes must be zero.
- Delta advances sample time, then the annotation is emitted at that sample.
- PC constructs annotation text as `label + rendered_data` and sends that
  directly to the annotation UI.
- The default render traces are named `LOG-DEBUG`, `LOG-INFO`, `LOG-WARN`,
  and `LOG-ERROR`; level 3 is the error channel.
- DSView colors direct annotations by log level.

HEX render:

```text
data byte 0xAB -> "AB"
```

ASCII render:

- Printable ASCII bytes `0x20..0x7E`, `\r`, `\n`, and `\t` are preserved.
- Other bytes are escaped as `\xNN`.
- The parser must not treat the payload as a NUL-terminated C string.

The direct text path does not synthesize 8N1 waveform and does not run the UART
protocol decoder. This is the primary v3 CPU optimization.

## 6. Mode 2: Sync / Resync

Header `0xA0` is a fixed 16-byte sync frame:

```text
[delta:3][0xA0][gpio_state32:4][inv_gpio_state32:4][0x55][0xAA][0x5A][0xA5]
```

Rules:

- `gpio_state32` carries the absolute D0-D27 level in bits 0..27; bits 28..31
  must be zero.
- `inv_gpio_state32` must be the bitwise inverse of `gpio_state32`.
- Tail bytes must be `0x55 0xAA 0x5A 0xA5`.
- The MCU must emit sync periodically every 200 ms. Normal GPIO and text
  frames do not carry sync overhead.
- On acquisition start, the PC drops all frames until the first valid sync
  frame. That first sync establishes sample 0 and its delta is ignored.
- On invalid framing, the PC drops bytes until the next valid sync frame, then
  resumes from the sync absolute state.

## 7. PC Data Path

The driver should emit two independent datafeed types:

- `SR_DF_LOGIC` with `LA_SPARSE_EVENTS` for D0-D27 GPIO state.
- `SR_DF_UART_VCD_TEXT` for direct RX text annotations.

Suggested text payload:

```c
struct sr_datafeed_uart_vcd_text {
    uint64_t start_sample;
    uint64_t end_sample;
    uint8_t channel;      /* 0..3 log level */
    uint8_t is_label;     /* unused in v3 extended, always 0 */
    uint8_t reserved[6];
    const char *text;     /* UTF-8/ASCII, valid during ds_data_forward() */
};
```

`SigSession::data_feed_in()` routes `SR_DF_UART_VCD_TEXT` to the decoder stack
whose first probe index is `28 + level`. If no matching stack exists, it
prints a diagnostic and drops the annotation. It must not inject text into an
unrelated decoder stack.

`DecoderStack::push_native_annotation()` must be public or wrapped by a public
method for this route.

## 8. MCU API

Keep the existing fast GPIO APIs:

```c
void gpio_event_high(int channel);
void gpio_event_low(int channel);
void gpio_event_toggle(int channel);
void gpio_event_irq_high(unsigned int channel);
void gpio_event_irq_low(unsigned int channel);
void gpio_event_irq_toggle(unsigned int channel);
```

Add v3 extended text/log API:

```c
typedef enum {
    GPIO_EVENT_LEVEL_DEBUG = 0,
    GPIO_EVENT_LEVEL_INFO = 1,
    GPIO_EVENT_LEVEL_WARN = 2,
    GPIO_EVENT_LEVEL_ERROR = 3,
} gpio_event_level_t;

typedef enum {
    GPIO_EVENT_RENDER_MODE_HEX = 0,
    GPIO_EVENT_RENDER_MODE_ASCII = 1,
} gpio_event_render_mode_t;

void gpio_event_send_text(gpio_event_level_t level,
                          gpio_event_render_mode_t render_mode,
                          const uint8_t *label, int label_len,
                          const uint8_t *data, int data_len);
```

## 9. Validation Rules

The PC parser must reject:

- unknown headers
- GPIO channel > 27
- log level > 3
- label and data both empty
- text frame with payload larger than protocol maximum
- non-zero padding
- malformed sync frame
- abnormal delta larger than the existing clamp, except first event handling

On bad packets, the PC prints a bounded hex dump, searches for the next valid
sync frame, drops only bytes before that sync frame, and continues acquisition.

## 10. Implementation Order

1. Add `SR_DF_UART_VCD_TEXT` and route it through `SigSession`.
2. Make native annotation injection callable from `SigSession`.
3. Add v3 extended direct log parser in `uart_vcd.c`.
4. Add MCU `gpio_event_send_text()`.
5. Update `app_dma.c` to use log events.
6. Test that RX annotations display after capture without native UART decode.
7. Only then revisit multi-GPIO mask or multi-string grouping.
