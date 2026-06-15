# Obsolete UART_VCD Protocol

This document described the removed varint delta/toggle-mask protocol.

The current driver and MCU firmware use protocol v2:

```text
[uint24_le delta_ticks][header][optional payload]
```

Use `test_uart_vcd_event_protocol_2.md` as the protocol reference.
`send_event_test.py` still generates the obsolete varint stream and is not
compatible with the current `uart_vcd.c` parser.
