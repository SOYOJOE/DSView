#!/usr/bin/env python3
"""UART_VCD Event Protocol Test Sender

Usage:
    python3 send_event_test.py /dev/ttyUSB0    # real serial port
    python3 send_event_test.py /tmp/vcom1       # virtual port (socat)

Protocol: varint delta_time + varint toggle_mask, delta_time=0 ends stream.
Tick: 1us, samplerate: 1MHz (auto-set by driver).
"""

import struct
import sys
import time

def encode_varint(value):
    """Encode uint into varint bytes (LE, 7 bits per byte, MSB=continuation)."""
    result = bytearray()
    while value > 0x7F:
        result.append((value & 0x7F) | 0x80)
        value >>= 7
    result.append(value & 0x7F)
    return bytes(result)

def make_event(delta_time, toggle_mask):
    """Return bytes for one event: [varint(delta)] [varint(mask)]."""
    return encode_varint(delta_time) + encode_varint(toggle_mask)

# ─── Test Scenario 1: Basic GPIO Toggle (3 channels) ───
SCENARIO_1 = b"".join([
    make_event(100, 0x00000008),   # t=100us:  GPIO3 rise
    make_event(100, 0x00000008),   # t=200us:  GPIO3 fall
    make_event(100, 0x00000080),   # t=300us:  GPIO7 rise
    make_event(50,  0x00000001),   # t=350us:  GPIO0 rise
    make_event(50,  0x00000081),   # t=400us:  GPIO7+GPIO0 fall
    make_event(100, 0x00000008),   # t=500us:  GPIO3 rise
    make_event(100, 0x00000008),   # t=600us:  GPIO3 fall
    b'\x00',                        # end of stream
])

# ─── Test Scenario 2: GPIO31 High Bit ───
SCENARIO_2 = b"".join([
    make_event(500, 0x80000000),   # t=500us:  GPIO31 rise
    make_event(500, 0x80000000),   # t=1000us: GPIO31 fall
    b'\x00',
])

# ─── Test Scenario 3: Dense Toggle ───
SCENARIO_3 = b"".join([
    make_event(10, 0x01),  # GPIO0 rise
    make_event(10, 0x02),  # GPIO1 rise
    make_event(10, 0x01),  # GPIO0 fall
    make_event(10, 0x02),  # GPIO1 fall
    make_event(10, 0x01),  # GPIO0 rise
    make_event(10, 0x02),  # GPIO1 rise
    make_event(10, 0x01),  # GPIO0 fall
    make_event(10, 0x02),  # GPIO1 fall
    make_event(10, 0x01),  # GPIO0 rise
    make_event(10, 0x02),  # GPIO1 rise
    b'\x00',
])

# ─── Test Scenario 4: Long Idle ───
SCENARIO_4 = b"".join([
    make_event(100,     0x00000020),  # t=100us:    GPIO5 rise
    make_event(100000,  0x00000020),  # t=100100us: GPIO5 fall
    b'\x00',
])

SCENARIOS = {
    "1": (SCENARIO_1, "Basic GPIO toggle (3ch, 600us)"),
    "2": (SCENARIO_2, "GPIO31 high bit toggle"),
    "3": (SCENARIO_3, "Dense toggle (10 events)"),
    "4": (SCENARIO_4, "Long idle (100ms gap)"),
}


def send_to_serial(port, data, baudrate=1000000):
    """Send data bytes to serial port."""
    try:
        import serial
        ser = serial.Serial(port, baudrate, timeout=1)
        ser.flushOutput()
        ser.flushInput()
        ser.write(data)
        ser.flush()
        ser.close()
        return True
    except ImportError:
        print("pyserial not installed, falling back to raw write", file=sys.stderr)
        with open(port, 'wb', buffering=0) as f:
            f.write(data)
        return True
    except Exception as e:
        print(f"Serial error: {e}", file=sys.stderr)
        return False


def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <serial_port> [scenario_number]")
        print(f"  e.g.: {sys.argv[0]} /dev/ttyUSB0 1")
        print()
        print("Scenarios:")
        for k, (data, desc) in SCENARIOS.items():
            print(f"  {k}: {desc} ({len(data)} bytes)")
        return

    port = sys.argv[1]
    scenario = sys.argv[2] if len(sys.argv) > 2 else "1"

    if scenario not in SCENARIOS:
        print(f"Unknown scenario: {scenario}", file=sys.stderr)
        return

    data, desc = SCENARIOS[scenario]
    print(f"Sending scenario {scenario}: {desc}")
    print(f"Port: {port}, baud: 1000000")
    print(f"Data ({len(data)} bytes): {data.hex(' ')}")
    print(f"Varint breakdown:")

    i = 0
    event_num = 0
    while i < len(data):
        if data[i] == 0 and i == len(data) - 1 - (0 if data[-1] != 0 else 0):
            print(f"  Event {event_num:2d}: delta=0 (END)")
            break

        val1, off1 = 0, 0
        while i < len(data):
            b = data[i]; i += 1
            val1 |= (b & 0x7F) << (off1 * 7); off1 += 1
            if not (b & 0x80):
                break

        val2, off2 = 0, 0
        while i < len(data):
            b = data[i]; i += 1
            val2 |= (b & 0x7F) << (off2 * 7); off2 += 1
            if not (b & 0x80):
                break

        print(f"  Event {event_num:2d}: delta={val1:>6d}, mask=0x{val2:08x}")
        event_num += 1

    if not send_to_serial(port, data):
        return

    print("Done. Check DSView waveform.")
    print()
    print("Expected for scenario 1 (- high, _ low):")
    print("  D0: low 0~350us, high 350~400us, low after")
    print("  D3: low 0~100us, high 100~200us, low 200~500us, high 500~600us")
    print("  D7: low 0~300us, high 300~400us, low after")


if __name__ == "__main__":
    main()
