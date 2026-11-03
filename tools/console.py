"""Serial log monitor that also sends console commands to the device (the battery simulation, DESIGN.md 6.6).

The firmware reads lines on its USB-Serial-JTAG console: "bat <mV>" sets the simulated battery voltage, "usb 0|1"
the simulated USB power, "bat" or "usb" alone print them. COM5 can only be opened once, so the log reader and the
sender have to be the same process: this one prints (and optionally logs) everything the device writes, sends the
--send commands, and forwards every line typed on stdin. It reconnects when the board restarts.

--send "CMD" sends CMD right after the port opens; --send "SECONDS:CMD" sends it that many seconds after the start.
--for SECONDS stops after that long (default: run until Ctrl+C). --log FILE appends the output to FILE.
--reset resets the board first (RTS, like esptool): a wake without pressing a button.

Usage: python console.py [PORT] [--reset] [--send CMD]... [--for SECONDS] [--log FILE]     (default port COM5)
Example: python console.py --send "bat 3450" --send "20:usb 1" --for 40
"""
import argparse
import re
import sys
import threading
import time

import serial
from serial.tools import list_ports


def port_present(name):
    return any(p.device == name for p in list_ports.comports())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port", nargs="?", default="COM5")
    ap.add_argument("--send", action="append", default=[])
    ap.add_argument("--for", dest="duration", type=float)
    ap.add_argument("--log")
    ap.add_argument("--reset", action="store_true")
    args = ap.parse_args()

    pending = []  # (time to send, command), in order
    for s in args.send:
        m = re.match(r"^(\d+(?:\.\d+)?):(.*)$", s)
        pending.append((float(m.group(1)), m.group(2)) if m else (0.0, s))
    pending.sort(key=lambda p: p[0])
    typed = []
    lock = threading.Lock()

    def read_stdin():
        for line in sys.stdin:
            with lock:
                typed.append(line.strip())

    if sys.stdin.isatty():
        threading.Thread(target=read_stdin, daemon=True).start()

    log = open(args.log, "a", encoding="utf-8") if args.log else None
    start = time.time()
    last_err = None
    while args.duration is None or time.time() - start < args.duration:
        try:
            port = serial.Serial()
            port.port, port.baudrate, port.timeout = args.port, 115200, 0.1
            port.dtr = port.rts = False  # no reset when the port opens
            port.open()
            with port:
                if args.reset:
                    args.reset = False
                    port.rts = True
                    port.dtr = False  # on Windows the RTS change only applies with a DTR write
                    time.sleep(0.1)
                    port.rts = False
                    port.dtr = False
                buf = b""
                last_rx = time.time()
                while args.duration is None or time.time() - start < args.duration:
                    now = time.time() - start
                    with lock:
                        out = typed[:]
                        typed.clear()
                    while pending and pending[0][0] <= now:
                        out.append(pending.pop(0)[1])
                    for cmd in out:
                        port.write((cmd + "\n").encode())
                        print(f">>> {cmd}", flush=True)
                        if log:
                            log.write(f">>> {cmd}\n")
                    data = port.read(4096)
                    if data:
                        last_rx = time.time()
                    elif time.time() - last_rx > 1:  # silent for a second: check the port list
                        if not port_present(args.port):
                            # deep sleep: the port is gone, but on Windows the open handle just stays silent
                            raise serial.SerialException("port gone")
                        last_rx = time.time()
                    buf += data
                    *done, buf = buf.split(b"\n")
                    for line in done:
                        text = line.decode("utf-8", errors="replace").rstrip("\r")
                        print(text, flush=True)
                        if log:
                            log.write(text + "\n")
                            log.flush()
        except serial.SerialException as e:  # the port goes away while the board restarts or sleeps
            if str(e) != last_err:
                last_err = str(e)
                print(f"[{time.time() - start:.1f}] {e}", file=sys.stderr, flush=True)
            time.sleep(0.5)
    if log:
        log.close()


if __name__ == "__main__":
    main()
