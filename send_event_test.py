#!/usr/bin/env python3
"""UART_VCD protocol v2 TCP test server.

DSView's uart-vcd driver is a TCP client. Start this script first, configure
DSView's TCP host to this machine, then start acquisition.

Examples:
    python3 send_event_test.py basic
    python3 send_event_test.py common
    python3 send_event_test.py max-wire --duration 10 --wire-mbps 3
    python3 send_event_test.py max-all --duration 2
    python3 send_event_test.py --list

Protocol:
    [uint24_le delta_ticks][header][optional payload]

Time base:
    24 MHz, one tick maps to one DSView sample.
"""

import argparse
import socket
import sys
import time


DEFAULT_HOST = "0.0.0.0"
DEFAULT_PORT = 12345
TICK_HZ = 24_000_000
UINT24_MAX = 0xFFFFFF

GPIO_LOW = 0
GPIO_HIGH = 1
GPIO_TOGGLE = 2

RENDER_HEX = 0
RENDER_ASCII = 1

UART_CHANNELS = 8
GPIO_CHANNELS = 24
UART_SAMPLES_PER_BIT = 4
UART_BITS_PER_BYTE = 10
UART_SAMPLES_PER_BYTE = UART_SAMPLES_PER_BIT * UART_BITS_PER_BYTE


def uint24_le(value):
    if not 0 <= value <= UINT24_MAX:
        raise ValueError(f"delta must be in range 0..{UINT24_MAX}")
    return bytes((value & 0xFF, (value >> 8) & 0xFF, (value >> 16) & 0xFF))


def gpio_event(delta, channel, level):
    if not 0 <= channel < GPIO_CHANNELS:
        raise ValueError("GPIO channel must be in range 0..23")
    if level not in (GPIO_LOW, GPIO_HIGH, GPIO_TOGGLE):
        raise ValueError("invalid GPIO level operation")
    header = (level << 5) | channel
    return uint24_le(delta) + bytes((header,))


def idle_event(delta):
    """Advance time without changing a valid channel.

    GPIO channel 31 is outside the implemented D0-D23 range, so the current
    DSView parser emits delta samples and ignores the state update.
    """
    return uint24_le(delta) + b"\x1f"


def string_event(delta, channel, render_mode, label=b"", data=b""):
    if not 0 <= channel < UART_CHANNELS:
        raise ValueError("UART channel must be in range 0..7")
    if render_mode not in (RENDER_HEX, RENDER_ASCII):
        raise ValueError("invalid render mode")

    label = bytes(label)
    data = bytes(data)
    if len(label) > 31:
        raise ValueError("label is limited to 31 bytes")
    if len(label) + len(data) > 255:
        raise ValueError("label + data is limited to 255 bytes")

    header = 0x80 | (render_mode << 5) | len(label)
    payload = bytes((channel, len(label) + len(data))) + label + data
    payload += bytes((-len(payload)) & 3)
    return uint24_le(delta) + bytes((header,)) + payload


def sync_event():
    """Consume the driver's first-event special case without changing state."""
    return idle_event(1)


def finite_stream(events, tail_ticks=UART_SAMPLES_PER_BYTE * 4):
    yield sync_event()
    yield from events
    if tail_ticks:
        yield idle_event(tail_ticks)


def scenario_basic(_args):
    events = [
        gpio_event(2_400, 3, GPIO_HIGH),   # 100 us
        gpio_event(2_400, 3, GPIO_LOW),
        gpio_event(2_400, 7, GPIO_HIGH),
        gpio_event(1_200, 0, GPIO_HIGH),
        gpio_event(1_200, 7, GPIO_LOW),
        gpio_event(0, 0, GPIO_LOW),
        gpio_event(2_400, 3, GPIO_HIGH),
        gpio_event(2_400, 3, GPIO_LOW),
    ]
    return finite_stream(events)


def scenario_gpio_walk(_args):
    events = []
    step = TICK_HZ // 2_000  # 0.5 ms
    for channel in range(GPIO_CHANNELS):
        events.append(gpio_event(step, channel, GPIO_HIGH))
        events.append(gpio_event(step, channel, GPIO_LOW))
    return finite_stream(events)


def scenario_uart(_args):
    events = []
    for channel in range(UART_CHANNELS):
        label = f"RX{channel}:".encode("ascii")
        if channel < 4:
            data = bytes((channel, 0x12, 0x80 + channel, 0xFF))
            events.append(string_event(2_400, channel, RENDER_HEX, label, data))
        else:
            data = f"hello-{channel}\r\n".encode("ascii")
            events.append(string_event(2_400, channel, RENDER_ASCII, label, data))
    return finite_stream(events, tail_ticks=20_000)


def scenario_long_idle(_args):
    events = [
        gpio_event(2_400, 5, GPIO_HIGH),
        gpio_event(2_400_000, 5, GPIO_LOW),  # 100 ms
    ]
    return finite_stream(events)


def scenario_common(args):
    def stream():
        yield sync_event()
        for factory in (scenario_basic, scenario_gpio_walk, scenario_uart,
                        scenario_long_idle):
            for packet in factory(args):
                if packet == sync_event():
                    continue
                yield packet
            yield idle_event(24_000)
    return stream()


def scenario_max_wire(args):
    """Minimum-size events, round-robin across all GPIO channels.

    Each event is 4 bytes and advances one sample. With --wire-mbps 3 this
    models the maximum event rate of a 3 Mbaud 8N1 UART: about 75k events/s,
    or 3,125 toggles/s per GPIO channel.
    """
    def stream():
        yield sync_event()
        channel = 0
        while True:
            yield gpio_event(1, channel, GPIO_TOGGLE)
            channel = (channel + 1) % GPIO_CHANNELS
    return stream()


def scenario_max_uart(_args):
    """Keep all eight virtual 6 Mbaud RX channels continuously active.

    A 48-byte ASCII burst occupies 1,920 samples. The next round is queued
    after exactly that interval, allowing every RX channel to run continuously
    without overflowing the driver's 64-byte FIFO under normal processing.
    """
    burst = bytes((ord("A") + (i % 26) for i in range(48)))

    def stream():
        yield sync_event()
        sequence = 0
        while True:
            for channel in range(UART_CHANNELS):
                delta = UART_SAMPLES_PER_BYTE * len(burst) if channel == 0 else 0
                data = bytes(((value + sequence + channel) & 0x7F) or 0x20
                              for value in burst)
                yield string_event(delta, channel, RENDER_ASCII, b"", data)
            sequence = (sequence + 1) & 0x1F
    return stream()


def scenario_max_all(_args):
    """Exercise all 32 output channels continuously.

    Every 40 samples all 24 GPIO channels toggle at the same timestamp and one
    ASCII byte is queued on each RX channel. This is the maximum waveform
    activity pattern, not a realistic 3 Mbaud UART transport load: the protocol
    stream is about 96 MB/s of TCP input.
    """
    def stream():
        yield sync_event()
        value = 0
        while True:
            for channel in range(GPIO_CHANNELS):
                yield gpio_event(UART_SAMPLES_PER_BYTE if channel == 0 else 0,
                                 channel, GPIO_TOGGLE)
            for channel in range(UART_CHANNELS):
                yield string_event(0, channel, RENDER_ASCII,
                                   data=bytes((0x20 + ((value + channel) % 95),)))
            value = (value + 1) % 95
    return stream()


SCENARIOS = {
    "basic": (
        "Basic GPIO timing on D0, D3 and D7",
        scenario_basic,
        False,
    ),
    "gpio-walk": (
        "Pulse each GPIO channel D0-D23 in sequence",
        scenario_gpio_walk,
        False,
    ),
    "uart": (
        "HEX on RX0-RX3 and ASCII on RX4-RX7",
        scenario_uart,
        False,
    ),
    "long-idle": (
        "D5 pulse with a 100 ms idle interval",
        scenario_long_idle,
        False,
    ),
    "common": (
        "Run all common functional tests",
        scenario_common,
        False,
    ),
    "max-wire": (
        "Maximum minimum-packet parser load across D0-D23",
        scenario_max_wire,
        True,
    ),
    "max-uart": (
        "Continuous maximum virtual UART load on RX0-RX7",
        scenario_max_uart,
        True,
    ),
    "max-all": (
        "Maximum waveform activity on all 32 channels",
        scenario_max_all,
        True,
    ),
}


class RateLimiter:
    def __init__(self, wire_mbps):
        self.bytes_per_second = wire_mbps * 1_000_000 / 10 if wire_mbps else 0
        self.started = time.monotonic()
        self.sent = 0

    def wait(self, byte_count):
        self.sent += byte_count
        if not self.bytes_per_second:
            return
        target = self.started + self.sent / self.bytes_per_second
        delay = target - time.monotonic()
        if delay > 0:
            time.sleep(delay)


def send_stream(conn, packets, duration, wire_mbps, report_interval):
    limiter = RateLimiter(wire_mbps)
    started = time.monotonic()
    report_at = started + report_interval
    total_bytes = 0
    total_events = 0
    interval_bytes = 0
    interval_events = 0

    for packet in packets:
        now = time.monotonic()
        if duration and now - started >= duration:
            break

        limiter.wait(len(packet))
        conn.sendall(packet)
        total_bytes += len(packet)
        total_events += 1
        interval_bytes += len(packet)
        interval_events += 1

        now = time.monotonic()
        if now >= report_at:
            elapsed = max(now - (report_at - report_interval), 1e-9)
            mbps = interval_bytes * 8 / elapsed / 1_000_000
            print(f"TX {mbps:8.2f} Mbps | {interval_events:9d} events | "
                  f"{total_bytes / 1_000_000:9.2f} MB total")
            interval_bytes = 0
            interval_events = 0
            report_at = now + report_interval

    elapsed = max(time.monotonic() - started, 1e-9)
    return total_bytes, total_events, elapsed


def wait_for_disconnect(conn):
    print("Stream complete. Stop acquisition in DSView to close the connection.")
    conn.settimeout(0.5)
    while True:
        try:
            data = conn.recv(1)
            if not data:
                return
        except socket.timeout:
            continue


def list_scenarios():
    print("Scenarios:")
    for name, (description, _factory, continuous) in SCENARIOS.items():
        suffix = " [continuous]" if continuous else ""
        print(f"  {name:12s} {description}{suffix}")


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("scenario", nargs="?", default="common",
                        choices=sorted(SCENARIOS))
    parser.add_argument("--host", default=DEFAULT_HOST,
                        help=f"listen address (default: {DEFAULT_HOST})")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT,
                        help=f"TCP port (default: {DEFAULT_PORT})")
    parser.add_argument("--duration", type=float, default=0,
                        help="stop sending after N wall-clock seconds")
    parser.add_argument("--wire-mbps", type=float, default=0,
                        help="pace output like an N-Mbaud 8N1 UART; 0 is unlimited")
    parser.add_argument("--report-interval", type=float, default=1.0,
                        help="throughput report interval in seconds")
    parser.add_argument("--repeat", action="store_true",
                        help="accept another DSView connection after disconnect")
    parser.add_argument("--close-after-send", action="store_true",
                        help="close TCP immediately after the selected stream")
    parser.add_argument("--list", action="store_true",
                        help="list scenarios and exit")
    args = parser.parse_args()

    if args.duration < 0:
        parser.error("--duration must be non-negative")
    if args.wire_mbps < 0:
        parser.error("--wire-mbps must be non-negative")
    if args.report_interval <= 0:
        parser.error("--report-interval must be positive")
    if not 1 <= args.port <= 65535:
        parser.error("--port must be in range 1..65535")
    return args


def serve(args):
    description, factory, continuous = SCENARIOS[args.scenario]
    if continuous and not args.duration:
        print("Continuous scenario selected; press Ctrl+C to stop.")

    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as server:
        server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        server.bind((args.host, args.port))
        server.listen(1)

        print(f"Scenario : {args.scenario} - {description}")
        print(f"TCP      : listening on {args.host}:{args.port}")
        print(f"Pacing   : {'unlimited TCP' if not args.wire_mbps else f'{args.wire_mbps:g} Mbaud 8N1'}")
        print("Start DSView acquisition now.")

        while True:
            conn, address = server.accept()
            with conn:
                conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                print(f"Connected: {address[0]}:{address[1]}")
                try:
                    total_bytes, total_events, elapsed = send_stream(
                        conn,
                        factory(args),
                        args.duration,
                        args.wire_mbps,
                        args.report_interval,
                    )
                    print(f"Finished : {total_bytes / 1_000_000:.3f} MB, "
                          f"{total_events} events, {elapsed:.3f} s, "
                          f"{total_bytes * 8 / elapsed / 1_000_000:.2f} Mbps")
                    if not args.close_after_send:
                        wait_for_disconnect(conn)
                except (BrokenPipeError, ConnectionResetError):
                    print("DSView disconnected.")

            if not args.repeat:
                break
            print("Waiting for the next DSView connection.")


def main():
    args = parse_args()
    if args.list:
        list_scenarios()
        return 0

    try:
        serve(args)
    except KeyboardInterrupt:
        print("\nStopped.")
    except OSError as exc:
        print(f"Socket error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
