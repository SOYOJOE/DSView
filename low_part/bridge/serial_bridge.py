#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""
serial_bridge.py

DSView UART -> TCP Bridge

Features
--------
- UART @ 3Mbps
- TCP Server
- DSView reconnect supported
- TCP disconnected => discard UART data
- TCP connected => realtime forwarding
- 1s statistics update
- Ctrl+C graceful exit
- Windows/Linux/macOS

Usage
-----
python serial_bridge.py [tcp_port]

Default TCP port:
12345
"""

import sys
import time
import socket
import serial
import threading
import serial.tools.list_ports


TCP_PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 12345
BAUD_RATE = 3000000

READ_SIZE = 4096


# ============================================================
# Statistics
# ============================================================

class Statistics:
    def __init__(self):
        self.lock = threading.Lock()

        self.rx_bytes = 0
        self.tx_bytes = 0

        self.client_connected = False

    def add_rx(self, size):
        with self.lock:
            self.rx_bytes += size

    def add_tx(self, size):
        with self.lock:
            self.tx_bytes += size

    def snapshot(self):
        with self.lock:
            rx = self.rx_bytes
            tx = self.tx_bytes

            self.rx_bytes = 0
            self.tx_bytes = 0

        return rx, tx


# ============================================================
# Serial
# ============================================================

def scan_serial_ports():
    return [p.device for p in serial.tools.list_ports.comports()]


def select_serial_port(ports):

    if not ports:
        print("No serial ports found.")
        sys.exit(1)

    print("\nAvailable serial ports:")

    for i, port in enumerate(ports):
        print(f"  [{i}] {port}")

    if len(ports) == 1:
        print(f"\nAuto-selected: {ports[0]}")
        return ports[0]

    while True:

        try:
            idx = int(
                input(
                    f"\nSelect port [0-{len(ports)-1}]: "
                )
            )

            if 0 <= idx < len(ports):
                return ports[idx]

        except Exception:
            pass

        print("Invalid selection.")


# ============================================================
# Shared Context
# ============================================================

class BridgeContext:

    def __init__(self):

        self.lock = threading.Lock()

        self.conn = None
        self.connected = False

    def set_connection(self, conn):

        with self.lock:
            self.conn = conn
            self.connected = True

    def clear_connection(self):

        with self.lock:

            if self.conn:
                try:
                    self.conn.close()
                except Exception:
                    pass

            self.conn = None
            self.connected = False

    def get_connection(self):

        with self.lock:
            return self.conn

    def is_connected(self):

        with self.lock:
            return self.connected


# ============================================================
# UART Thread
# ============================================================

def uart_worker(
        ser,
        ctx,
        stats,
        stop_event
):

    while not stop_event.is_set():

        try:

            data = ser.read(READ_SIZE)

            if not data:
                continue

            stats.add_rx(len(data))

            if not ctx.is_connected():
                # TCP断开时直接丢弃
                continue

            conn = ctx.get_connection()

            if conn is None:
                continue

            try:

                conn.sendall(data)

                stats.add_tx(len(data))

            except (
                    BrokenPipeError,
                    ConnectionResetError,
                    OSError
            ):

                ctx.clear_connection()
                stats.client_connected = False

        except serial.SerialException as e:

            print(f"\nUART error: {e}")

            stop_event.set()
            break


# ============================================================
# Status Thread
# ============================================================

def status_worker(
        stats,
        stop_event
):

    while not stop_event.wait(1):

        rx, tx = stats.snapshot()

        rx_mbps = rx * 8 / 1000000
        tx_mbps = tx * 8 / 1000000

        state = (
            "CONNECTED"
            if stats.client_connected
            else "WAITING"
        )

        print(
            "\r"
            f"UART RX: {rx_mbps:6.2f} Mbps | "
            f"TCP TX: {tx_mbps:6.2f} Mbps | "
            f"{state:10}",
            end="",
            flush=True
        )


# ============================================================
# Main
# ============================================================

def main():

    serial_port = select_serial_port(
        scan_serial_ports()
    )

    try:

        ser = serial.Serial(
            port=serial_port,
            baudrate=BAUD_RATE,
            bytesize=serial.EIGHTBITS,
            parity=serial.PARITY_NONE,
            stopbits=serial.STOPBITS_ONE,
            timeout=0.05
        )

    except Exception as e:

        print(f"Open serial failed: {e}")
        return

    print(f"\nSerial : {serial_port}")
    print(f"Baud   : {BAUD_RATE}")

    ctx = BridgeContext()

    stats = Statistics()

    stop_event = threading.Event()

    threading.Thread(
        target=uart_worker,
        args=(
            ser,
            ctx,
            stats,
            stop_event
        ),
        daemon=True
    ).start()

    threading.Thread(
        target=status_worker,
        args=(
            stats,
            stop_event
        ),
        daemon=True
    ).start()

    srv = socket.socket(
        socket.AF_INET,
        socket.SOCK_STREAM
    )

    srv.setsockopt(
        socket.SOL_SOCKET,
        socket.SO_REUSEADDR,
        1
    )

    srv.bind(("0.0.0.0", TCP_PORT))
    srv.listen(1)

    srv.settimeout(0.5)

    print(f"TCP    : Listen on {TCP_PORT}")
    print("\nWaiting for DSView ...")

    try:

        while not stop_event.is_set():

            try:

                conn, addr = srv.accept()

            except socket.timeout:
                continue

            print(
                f"\nClient connected: "
                f"{addr[0]}:{addr[1]}"
            )

            #
            # 丢弃断线期间积压的数据
            #
            try:
                ser.reset_input_buffer()
            except Exception:
                pass

            conn.setsockopt(
                socket.IPPROTO_TCP,
                socket.TCP_NODELAY,
                1
            )

            ctx.set_connection(conn)

            stats.client_connected = True

            #
            # 等待连接断开
            #
            while (
                    not stop_event.is_set()
                    and ctx.is_connected()
            ):
                time.sleep(0.1)

            stats.client_connected = False

            print("\nClient disconnected")

    except KeyboardInterrupt:

        print("\n\nCtrl+C received")

    finally:

        stop_event.set()

        ctx.clear_connection()

        try:
            srv.close()
        except Exception:
            pass

        try:
            ser.close()
        except Exception:
            pass

        print("Shutdown complete")


if __name__ == "__main__":
    main()
