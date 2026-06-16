# UART_VCD Event Protocol v3

## 1. Goal

v3 keeps the low-cost GPIO event path from v2, and replaces RX0-RX7 virtual
8N1 waveform synthesis with direct text annotations. The first implementation
is intentionally small:

- mode 0: single GPIO high/low event
- mode 1: RX label registration
- mode 3: direct RX text annotation

Mode 2 multi-GPIO mask is reserved until its timing semantics are fully
validated. It must not be used for interrupt traces where every edge and edge
order matters.

Every on-wire frame starts with a 2-byte sync word. This removes the v2/v3
ambiguity where text payload bytes could be reinterpreted as valid GPIO
events after one dropped byte.

```text
[0xA5][0x5A][uint24_le delta_ticks:3][header:1][payload...]
```

`delta_ticks` is the elapsed 24 MHz MCU systimer tick count since the previous
event that advances sample time. The PC maps one tick to one sample.

## 2. Header Map

Only these header values are valid in v3:

| Header | Mode | Meaning |
|---:|---|---|
| `0x00-0x17` | mode 0 | GPIO low, channel 0-23 |
| `0x20-0x37` | mode 0 | GPIO high, channel 0-23 |
| `0x80` | mode 1 | label registration |
| `0xA0` | reserved | future multi-GPIO mask |
| `0xC0` | mode 3 | direct text, HEX render |
| `0xE0` | mode 3 | direct text, ASCII render |

All other headers are invalid and must be rejected by the PC parser. In
particular, v2 string headers such as `0x86` are not valid v3 frames.

## 3. Mode 0: GPIO Single

```text
[0xA5][0x5A][delta:3][header:1]
header = (sub << 5) | channel
sub = 0 low, 1 high
channel = 0..23
```

Rules:

- Frame size is always 6 bytes.
- MCU APIs may keep `toggle`, but it must be converted locally to absolute
  high/low before transmission.
- Delta is emitted before the state change: idle time first, then new level.
- This mode is safe for ISR printing because it preserves every edge.

## 4. Mode 1: Label Registration

```text
[0xA5][0x5A][delta:3][0x80][channel:1][label_len:1][label:label_len][pad_to_4B]
```

Rules:

- `channel` is 0..7 for RX0-RX7.
- `label_len` is 0..127.
- Payload length is `2 + label_len`, padded to 4 bytes with zero bytes.
- Frame size is `6 + align4(2 + label_len)`.
- Padding bytes must be zero.
- Label bytes are printable ASCII. Non-printable bytes should be escaped by
  the PC if accepted.
- Label registration is metadata. The recommended MCU delta is 0. The PC may
  accept non-zero delta for resync tolerance, but label events should not be
  used to represent user signal timing.
- The PC stores the label in the active capture context only. Do not persist
  labels to disk in the first implementation; the MCU should send labels after
  each reset/start.

The final text prefix is exactly the registered label. If a separator is
desired, include it in the label, for example `RX0:`. The PC must not add an
extra colon automatically.

## 5. Mode 3: Direct Text Annotation

```text
[0xA5][0x5A][delta:3][header:1][channel:1][data_len:1][data:data_len][pad_to_4B]
header = 0xC0 for HEX render
header = 0xE0 for ASCII render
```

Rules:

- `channel` is 0..7 for RX0-RX7.
- `data_len` is 0..255.
- Payload length is `2 + data_len`, padded to 4 bytes with zero bytes.
- Frame size is `6 + align4(2 + data_len)`.
- Padding bytes must be zero.
- Delta advances sample time, then the annotation is emitted at that sample.
- Annotation text is `label + rendered_data`.
- If no label has been registered, use default labels `RX0:`..`RX7:`.

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

## 6. Reserved Mode 2: Multi-GPIO Mask

Header `0xA0` is reserved for a future 12-byte aligned frame:

```text
[0xA5][0x5A][delta:3][0xA0][changed_mask:3][level_mask:3][pad:2]
```

It is not part of the first implementation.

Before enabling it, these semantics must be decided and tested:

- It represents final levels for a group of channels at one timestamp.
- It does not preserve multiple edges on the same channel within the group.
- It does not preserve ordering between changed channels.
- If implemented on MCU ISR paths without writing DMA in the ISR, pending
  batches need a small queue; one global pending mask is not enough when a
  second batch starts before the main loop flushes the first.

## 7. PC Data Path

The driver should emit two independent datafeed types:

- `SR_DF_LOGIC` with `LA_SPARSE_EVENTS` for D0-D23 GPIO state.
- `SR_DF_UART_VCD_TEXT` for direct RX text annotations.

Suggested text payload:

```c
struct sr_datafeed_uart_vcd_text {
    uint64_t start_sample;
    uint64_t end_sample;
    uint8_t channel;      /* 0..7, RX0-RX7 */
    uint8_t reserved[7];
    const char *text;     /* UTF-8/ASCII, valid during ds_data_forward() */
};
```

`SigSession::data_feed_in()` routes `SR_DF_UART_VCD_TEXT` to the decoder stack
whose first probe index is `24 + channel`. If no matching stack exists, it
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

Add v3 text APIs:

```c
void gpio_event_send_label(int channel, const uint8_t *label, int label_len);
void gpio_event_send_text(int channel, int render_mode,
                          const uint8_t *data, int data_len);
```

## 9. Validation Rules

The PC parser must reject:

- unknown headers
- missing `A5 5A` sync word
- GPIO channel > 23
- RX channel > 7
- label length > 127
- text frame with payload larger than protocol maximum
- non-zero padding
- reserved mode 2 frames until implemented
- abnormal delta larger than the existing clamp, except first event handling

On bad packets, the PC prints a bounded hex dump, searches for the next valid
frame, drops only the bad bytes, and continues acquisition.

## 10. Implementation Order

1. Add `SR_DF_UART_VCD_TEXT` and route it through `SigSession`.
2. Make native annotation injection callable from `SigSession`.
3. Add v3-only label/text parser in `uart_vcd.c`.
4. Add MCU `gpio_event_send_label()` and `gpio_event_send_text()`.
5. Update `app_dma.c` to send labels once and use text events.
6. Test that RX annotations display after capture without native UART decode.
7. Only then revisit reserved mode 2 or multi-string grouping.
