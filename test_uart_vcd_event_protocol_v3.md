# UART_VCD Event Protocol v3

## 1. Goal

v3 keeps the low-cost GPIO event path from v2, and replaces RX0-RX7 virtual
8N1 waveform synthesis with direct text annotations. The first implementation
is intentionally small:

- mode 0: single GPIO high/low event
- mode 1: RX label registration
- mode 2: sync/resync absolute GPIO state
- mode 3: direct RX text annotation

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
| `0x00-0x17` | mode 0 | GPIO low, channel 0-23 |
| `0x20-0x37` | mode 0 | GPIO high, channel 0-23 |
| `0x80` | mode 1 | label registration |
| `0xA0` | mode 2 | sync/resync, absolute GPIO state |
| `0xC0` | mode 3 | direct text, HEX render |
| `0xE0` | mode 3 | direct text, ASCII render |

All other headers are invalid and must be rejected by the PC parser. In
particular, v2 string headers such as `0x86` are not valid v3 frames.

## 3. Mode 0: GPIO Single

```text
[delta:3][header:1]
header = (sub << 5) | channel
sub = 0 low, 1 high
channel = 0..23
```

Rules:

- Frame size is always 4 bytes.
- MCU APIs may keep `toggle`, but it must be converted locally to absolute
  high/low before transmission.
- Delta is emitted before the state change: idle time first, then new level.
- This mode is safe for ISR printing because it preserves every edge.

## 4. Mode 1: Label Registration

```text
[delta:3][0x80][channel:1][label_len:1][label:label_len][pad_to_4B]
```

Rules:

- `channel` is 0..7 for RX0-RX7.
- `label_len` is 0..127.
- Payload length is `2 + label_len`, padded to 4 bytes with zero bytes.
- Frame size is `4 + align4(2 + label_len)`.
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
[delta:3][header:1][channel:1][data_len:1][data:data_len][pad_to_4B]
header = 0xC0 for HEX render
header = 0xE0 for ASCII render
```

Rules:

- `channel` is 0..7 for RX0-RX7.
- `data_len` is 0..255.
- Payload length is `2 + data_len`, padded to 4 bytes with zero bytes.
- Frame size is `4 + align4(2 + data_len)`.
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

## 6. Mode 2: Sync / Resync

Header `0xA0` is a fixed 12-byte sync frame:

```text
[delta:3][0xA0][gpio_state24:3][inv_gpio_state24:3][0x55][0xAA]
```

Rules:

- `gpio_state24` is the absolute D0-D23 level after advancing delta.
- `inv_gpio_state24` must be the bitwise inverse over 24 bits.
- Tail bytes must be `0x55 0xAA`.
- The MCU should emit sync periodically, for example once per 1 ms reporting
  cycle. Normal GPIO and text frames do not carry sync overhead.
- On invalid framing, the PC drops bytes until the next valid sync frame, then
  resumes from the sync absolute state.

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
- GPIO channel > 23
- RX channel > 7
- label length > 127
- text frame with payload larger than protocol maximum
- non-zero padding
- malformed sync frame
- abnormal delta larger than the existing clamp, except first event handling

On bad packets, the PC prints a bounded hex dump, searches for the next valid
sync frame, drops only bytes before that sync frame, and continues acquisition.

## 10. Implementation Order

1. Add `SR_DF_UART_VCD_TEXT` and route it through `SigSession`.
2. Make native annotation injection callable from `SigSession`.
3. Add v3-only label/text parser in `uart_vcd.c`.
4. Add MCU `gpio_event_send_label()` and `gpio_event_send_text()`.
5. Update `app_dma.c` to send labels once and use text events.
6. Test that RX annotations display after capture without native UART decode.
7. Only then revisit multi-GPIO mask or multi-string grouping.
