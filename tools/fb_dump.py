"""Save the frames a SCREEN_DUMP build prints (`idf.py -DSCREEN_DUMP=1 build`) as PNGs.

The firmware prints every frame it sends to the panel as "FB begin <state>", one line per pixel row that is not
all white, "FB <row> <byte>:<hex> ..." (runs of bytes other than 0xFF, 1 = white), and "FB end". A line cut
by another task's log output is lost, so that row comes out white. This reads them from the serial port (or from a saved log file) and writes
fb_<n>_<state>.png into OUT_DIR (default %TEMP%/gl_test/frames). Other lines are echoed, so it also serves as
a log monitor. Needs pyserial and Pillow.

At boot a SCREEN_DUMP build also dumps every screen once with sample content (frame_dump_samples() in
main/frame_dump.c); --reset resets the board first (RTS, like esptool) to get them.

Usage: python fb_dump.py [--reset] [PORT_OR_LOGFILE] [OUT_DIR]     (default port COM5; Ctrl+C to stop)
"""
import os
import sys
import tempfile
import time

from PIL import Image

WIDTH, HEIGHT = 400, 300


def lines_from(source, reset):
    if os.path.isfile(source):
        with open(source, encoding="utf-8", errors="replace") as f:
            yield from f
        return
    import serial

    while True:
        try:
            with serial.Serial(source, 115200, timeout=1) as port:
                if reset:
                    reset = False
                    port.dtr = False
                    port.rts = True
                    time.sleep(0.1)
                    port.rts = False
                buf = b""
                while True:
                    buf += port.read(4096)
                    *done, buf = buf.split(b"\n")
                    for line in done:
                        yield line.decode("utf-8", errors="replace")
        except serial.SerialException:  # the port goes away while the board resets
            time.sleep(0.5)


def parse_row(line):
    """An "FB <row> <byte>:<hex> ..." line as (row, [(first byte, hex), ...]), or None if it is broken (cut by
    other output)."""
    try:
        _, y, *items = line.split()
        runs = [(int(start), data) for start, data in (item.split(":") for item in items)]
    except ValueError:
        return None
    if not all(len(data) % 2 == 0 and start + len(data) // 2 <= WIDTH // 8 for start, data in runs):
        return None
    return int(y), runs


def frame_image(rows):
    """The frame as a 1-bit image; rows that never arrived stay white."""
    image = Image.new("1", (WIDTH, HEIGHT), 1)
    pixels = image.load()
    for y, runs in rows.items():
        for start, data in runs:
            for i, byte in enumerate(bytes.fromhex(data)):
                for bit in range(8):
                    pixels[(start + i) * 8 + bit, y] = (byte >> (7 - bit)) & 1
    return image


def main():
    reset = "--reset" in sys.argv
    args = [a for a in sys.argv[1:] if a != "--reset"]
    source = args[0] if args else "COM5"
    out_dir = args[1] if len(args) > 1 else os.path.join(tempfile.gettempdir(), "gl_test", "frames")
    os.makedirs(out_dir, exist_ok=True)
    rows, label, frame_count = None, "", 0
    for line in lines_from(source, reset):
        line = line.rstrip("\r\n")
        if line.startswith("FB begin"):
            rows, label = {}, line[9:].strip() or "frame"
        elif line == "FB end" and rows is not None:
            frame_count += 1
            path = os.path.join(out_dir, f"fb_{frame_count:03d}_{label}.png")
            frame_image(rows).save(path)
            print(f"saved {path} ({len(rows)} rows)")
            rows = None
        elif line.startswith("FB ") and rows is not None:
            row = parse_row(line)
            if row:
                rows[row[0]] = row[1]
        else:
            print(line)


if __name__ == "__main__":
    main()
