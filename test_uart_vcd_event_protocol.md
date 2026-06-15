# Obsolete UART_VCD Protocol

This document described the removed varint delta/toggle-mask protocol.

The current driver and MCU firmware use protocol v2:

```text
[uint24_le delta_ticks][header][optional payload]
```

Use `test_uart_vcd_event_protocol_2.md` as the protocol reference.
`send_event_test.py` is the protocol v2 TCP test server. This obsolete
document is retained only to record that the varint protocol was removed.
