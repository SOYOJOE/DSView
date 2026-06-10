
#!/usr/bin/env python3
"""
serial_bridge.py

MCU UART -> RingBuffer -> TCP (DSView)

Features:
    - UART always running
    - Fixed-size ring buffer
    - Old data overwritten when full
    - TCP reconnect supported
    - New client receives latest data only
"""

import sys
import time
import socket
import serial
import threading
import serial.tools.list_ports

TCP_PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 12345
BAUD_RATE = 3000000

RING_BUFFER_SIZE = 1024 * 1024  # 1MB


# ============================================================
# Ring Buffer
# ============================================================

class RingBuffer:
    def __init__(self, size):
        self.size = size
        self.buf = bytearray(size)

        self.write_pos = 0
        self.total_written = 0

        self.lock = threading.Lock()

    def write(self, data):
        n = len(data)

        with self.lock:

            if n >= self.size:
                data = data[-self.size:]
                n = len(data)

            end = self.write_pos + n

            if end <= self.size:
                self.buf[self.write_pos:end] = data
            else:
                first = self.size - self.write_pos
                self.buf[self.write_pos:] = data[:first]
                self.buf[:end % self.size] = data[first:]

            self.write_pos = end % self.size
            self.total_written += n

    def get_latest_position(self):
        with self.lock:
            return self.total_written

    def read_from(self, position, max_len=65536):

        with self.lock:

            available = self.total_written - position

            if available <= 0:
                return b'', position

            if available > self.size:
                position = self.total_written - self.size
                available = self.size

            length = min(available, max_len)

            start = position % self.size
            end = start + length

            if end <= self.size:
                data = bytes(self.buf[start:end])
            else:
                first = self.size - start
                data = (
                        bytes(self.buf[start:])
                        + bytes(self.buf[:end % self.size])
                )

            return data, position + length


# ============================================================
# Serial
# ============================================================

def scan_serial_ports():
    return [p.device for p in serial.tools.list_ports.comports()]


def select_serial_port(ports):

    if not ports:
        print("No serial ports found")
        sys.exit(1)

    print("\nAvailable serial ports:")

    for i, p in enumerate(ports):
        print(f" [{i}] {p}")

    if len(ports) == 1:
        print(f"Auto-selected: {ports[0]}")
        return ports[0]

    while True:

        try:
            idx = int(input("Select port: "))

            if 0 <= idx < len(ports):
                return ports[idx]

        except Exception:
            pass


# ============================================================
# UART Reader
# ============================================================

def uart_reader(ser, ring):

    rx_bytes = 0
    last_tick = time.time()

    while True:

        try:
            data = ser.read(16384)

            if data:
                ring.write(data)
                rx_bytes += len(data)

            now = time.time()

            if now - last_tick >= 1:

                mbps = rx_bytes * 8 / 1000000

                print(
                    f"\rUART RX: {mbps:.2f} Mbps",
                    end="",
                    flush=True
                )

                rx_bytes = 0
                last_tick = now

        except serial.SerialException as e:

            print(f"\nSerial error: {e}")

            time.sleep(1)


# ============================================================
# TCP Sender
# ============================================================

def tcp_stream_client(conn, ring):

    try:

        conn.setsockopt(socket.IPPROTO_TCP,
                        socket.TCP_NODELAY,
                        1)

        # 从最新位置开始
        position = ring.get_latest_position()

        tx_bytes = 0
        last_tick = time.time()

        while True:

            data, position = ring.read_from(
                position,
                max_len=32768
            )

            if not data:
                time.sleep(0.001)
                continue

            conn.sendall(data)

            tx_bytes += len(data)

            now = time.time()

            if now - last_tick >= 1:

                mbps = tx_bytes * 8 / 1000000

                print(
                    f"\rTCP TX : {mbps:.2f} Mbps",
                    end="",
                    flush=True
                )

                tx_bytes = 0
                last_tick = now

    except (
            BrokenPipeError,
            ConnectionResetError,
            OSError
    ):
        pass

    finally:

        try:
            conn.close()
        except:
            pass

        print("\nClient disconnected")


# ============================================================
# Main
# ============================================================

def main():

    port = select_serial_port(scan_serial_ports())

    try:

        ser = serial.Serial(
            port,
            BAUD_RATE,
            bytesize=serial.EIGHTBITS,
            parity=serial.PARITY_NONE,
            stopbits=serial.STOPBITS_ONE,
            timeout=0.05
        )

    except Exception as e:

        print(e)
        return

    print(f"Serial : {port}")
    print(f"Baud   : {BAUD_RATE}")

    ring = RingBuffer(RING_BUFFER_SIZE)

    threading.Thread(
        target=uart_reader,
        args=(ser, ring),
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

    print(f"TCP listen : {TCP_PORT}")

    try:

        while True:

            print("\nWaiting for DSView...")

            conn, addr = srv.accept()

            print(
                f"Client connected: "
                f"{addr[0]}:{addr[1]}"
            )

            tcp_stream_client(
                conn,
                ring
            )

    except KeyboardInterrupt:

        pass

    finally:

        srv.close()
        ser.close()


if __name__ == "__main__":
    main()

