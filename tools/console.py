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


def parse_schedule(sends):
    """--send values as (seconds after the start, command), in order."""
    schedule = []
    for send in sends:
        match = re.match(r"^(\d+(?:\.\d+)?):(.*)$", send)
        schedule.append((float(match.group(1)), match.group(2)) if match else (0.0, send))
    schedule.sort(key=lambda item: item[0])
    return schedule


def open_port(name):
    port = serial.Serial()
    port.port, port.baudrate, port.timeout = name, 115200, 0.1
    port.dtr = port.rts = False  # no reset when the port opens
    port.open()
    return port


def reset_board(port):
    """Pulse RTS, like esptool: the board resets."""
    port.rts = True
    port.dtr = False  # on Windows the RTS change only applies with a DTR write
    time.sleep(0.1)
    port.rts = False
    port.dtr = False


class Console:
    def __init__(self, args):
        self.args = args
        self.schedule = parse_schedule(args.send)
        self.typed = []  # lines typed on stdin, not sent yet
        self.typed_lock = threading.Lock()
        self.log = open(args.log, "a", encoding="utf-8") if args.log else None
        self.start = time.time()

    def running(self):
        return self.args.duration is None or time.time() - self.start < self.args.duration

    def read_stdin(self):
        for line in sys.stdin:
            with self.typed_lock:
                self.typed.append(line.strip())

    def output(self, text):
        print(text, flush=True)
        if self.log:
            self.log.write(text + "\n")
            self.log.flush()

    def due_commands(self):
        """The typed lines, then the scheduled commands whose time has come."""
        with self.typed_lock:
            commands = self.typed[:]
            self.typed.clear()
        elapsed = time.time() - self.start
        while self.schedule and self.schedule[0][0] <= elapsed:
            commands.append(self.schedule.pop(0)[1])
        return commands

    def serve(self, port):
        """Send commands and print the log until the time is up or the port goes away."""
        buffer = b""
        last_received = time.time()
        while self.running():
            for command in self.due_commands():
                port.write((command + "\n").encode())
                self.output(f">>> {command}")
            data = port.read(4096)
            if data:
                last_received = time.time()
            elif time.time() - last_received > 1:  # silent for a second: check the port list
                if not port_present(self.args.port):
                    # deep sleep: the port is gone, but on Windows the open handle just stays silent
                    raise serial.SerialException("port gone")
                last_received = time.time()
            buffer += data
            *lines, buffer = buffer.split(b"\n")
            for line in lines:
                self.output(line.decode("utf-8", errors="replace").rstrip("\r"))

    def run(self):
        if sys.stdin.isatty():
            threading.Thread(target=self.read_stdin, daemon=True).start()
        last_error = None
        while self.running():
            try:
                with open_port(self.args.port) as port:
                    if self.args.reset:
                        self.args.reset = False
                        reset_board(port)
                    self.serve(port)
            except serial.SerialException as e:  # the port goes away while the board restarts or sleeps
                if str(e) != last_error:
                    last_error = str(e)
                    print(f"[{time.time() - self.start:.1f}] {e}", file=sys.stderr, flush=True)
                time.sleep(0.5)
        if self.log:
            self.log.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("port", nargs="?", default="COM5")
    parser.add_argument("--send", action="append", default=[])
    parser.add_argument("--for", dest="duration", type=float)
    parser.add_argument("--log")
    parser.add_argument("--reset", action="store_true")
    Console(parser.parse_args()).run()


if __name__ == "__main__":
    main()
