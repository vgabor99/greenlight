"""Dataset + view write test (docs/zigbee.md#input-ha-to-device; docs/research/wake-delivery-bench.md).

Default mode: reset the device once and wait for its wake report. Then, per round, make sure the device has the
test dataset (DS_LINES entries, SPACING ms apart, then the commit; skipped while the device already has it) and
push one view: the four view_* attributes in one write, random bits, the current local time and a new id. Watch
the device log for the verdict. Rounds are GAP s apart so Z2M's retries of one round never overlap the next.

--reset-each: also reset the device before every round (the real wake path: every wake is a reboot + rejoin),
and only publish after Z2M has received the wake report, as the HA automation does.

Env: MQTT_HOST (default 192.168.0.32), MQTT_PORT (1883), MQTT_USER, MQTT_PASS.
--settle=S: after the wake trigger, wait S seconds before the first write (default 0.5).
The wake trigger is a change of wake_count in the device's Z2M state, as in the HA
automation (docs/home-assistant.md#push-automation), not the serial log.

--ds-lines=N: entries in the test dataset, 1 to 32 (default 8).
--hu: Hungarian texts (UTF-8, every accented letter), cut to 64 bytes at a character boundary.
--hu-long: like --hu, but every other line's name is accented letters up to 64 bytes (3 ds_entry parts).
--drop-part: leave out the last part of the first multi-part line; expect the commit to be REJECTED.
Lines are in the line format II|name|state (docs/data-model.md#line-format), one icon per line.
Every line is sent in parts of at most 26 bytes (docs/zigbee.md#input-ha-to-device), split at character boundaries.
--resync-each: a new dataset (new texts, new hash) in every round, then the view: tests the multi-frame path.
--oos: views name a dataset the device does not have; expect "out of sync" and nothing drawn.
--same: the view id stays the same after round 1; expect "time only" (the footer time updates).
--no-view: send no view; the device should stay awake VIEW_WAIT_MS (45 s) instead of IDLE_TIMEOUT_MS (20 s).
Use GAP_S >= 50.

Usage: python paced_write_test.py ROUNDS [SPACING_MS] [GAP_S] [--reset-each] [--settle=S] [--ds-lines=N] [--hu]
       [--hu-long] [--drop-part] [--resync-each] [--oos] [--same] [--no-view]
(The rev 5 snapshot mode, --lines=N, is in git history before the rev 6 firmware.)
Full serial and Z2M logs go to paced_write_test_<date>_<time>.log in LOG_DIR (default %TEMP%/gl_test).
"""
import calendar
import hashlib
import json
import os
import random
import subprocess
import sys
import threading
import time
from dataclasses import dataclass

import paho.mqtt.client as mqtt
import serial

PORT = "COM5"
IDF_PYTHON = r"C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe"
DEVICE = "gf_lobby_greenlight"
DEVICE_IEEE = "0x10bda3fffea02724"
CUSTOM_CLUSTER = 0xFC8A

# Custom cluster attributes (docs/zigbee.md#input-ha-to-device) and ZCL data types
VIEW_BITS, VIEW_DS, VIEW_TIME, VIEW_ID, DS_ENTRY, DS_COMMIT = 0x20, 0x21, 0x22, 0x23, 0x30, 0x31
UINT32, STRING = 35, 66

# Dataset limits (ZIGBEE_LINE_MAX_BYTES, ZIGBEE_PART_MAX_BYTES, ZIGBEE_LINE_MAX_PARTS in main/zigbee/zigbee.h)
LINE_MAX_BYTES, PART_MAX_BYTES, LINE_MAX_PARTS = 64, 26, 3

HU_TEXTS = ["ÁÉÍÓÖŐÚÜŰ", "áéíóöőúüű", "Garázsajtó nyitva", "Hűtőajtó nyitva", "Mosógép kész",
            "Árvíztűrő tükörfúrógép"]   # 30 bytes: two parts
HU_LONG = "ÁÉÍÓÖŐÚÜŰáéíóöőúüűÁÉÍÓÖŐÚÜŰáéíóö"   # 32 accented letters: as a name, cut to a 64-byte line, three parts


# ---- Options ----

@dataclass
class Options:
    rounds: int
    spacing_s: float
    gap_s: float
    reset_each: bool
    settle_s: float
    dataset_lines: int
    hungarian: bool
    hungarian_long: bool
    drop_part: bool
    resync_each: bool
    out_of_sync: bool
    same_view: bool
    no_view: bool


def parse_options(argv):
    positional = [a for a in argv if not a.startswith("--")]

    def value(name, default, convert):
        return next((convert(a.split("=", 1)[1]) for a in argv if a.startswith(f"--{name}=")), default)

    options = Options(
        rounds=int(positional[0]) if len(positional) > 0 else 10,
        spacing_s=(int(positional[1]) if len(positional) > 1 else 100) / 1000.0,
        gap_s=float(positional[2]) if len(positional) > 2 else 15.0,
        reset_each="--reset-each" in argv,
        settle_s=value("settle", 0.5, float),
        dataset_lines=value("ds-lines", 8, int),
        hungarian="--hu" in argv,
        hungarian_long="--hu-long" in argv,
        drop_part="--drop-part" in argv,
        resync_each="--resync-each" in argv,
        out_of_sync="--oos" in argv,
        same_view="--same" in argv,
        no_view="--no-view" in argv,
    )
    if not 1 <= options.dataset_lines <= 32:
        sys.exit("--ds-lines must be 1 to 32")
    return options


# ---- The run's log file ----

class TestLog:
    def __init__(self, argv):
        log_dir = os.environ.get("LOG_DIR", os.path.join(os.environ.get("TEMP", "."), "gl_test"))
        os.makedirs(log_dir, exist_ok=True)
        self.file = open(os.path.join(log_dir, time.strftime("paced_write_test_%Y%m%d_%H%M%S.log")), "w",
                         encoding="utf-8")
        self.start = time.perf_counter()
        self.file.write(f"# args: {' '.join(argv)}\n")

    def write(self, source, text):
        self.file.write(f"{time.perf_counter() - self.start:9.3f} {source} {text}\n")
        self.file.flush()


# ---- MQTT: Z2M log lines about the device, and the device's own state messages ----

class Z2mMonitor:
    def __init__(self, log):
        self.log = log
        self.errors = []             # (t, message) for Z2M warnings/errors about the device
        self.wake_changes = []       # (t, value) of wake_count in every device message
        self.last_wake_count = None
        self.last_dataset_hash = None  # the device's dataset_hash as Z2M last published it
        self.client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
        if os.environ.get("MQTT_USER"):
            self.client.username_pw_set(os.environ["MQTT_USER"], os.environ.get("MQTT_PASS"))
        self.connected = threading.Event()
        self.client.on_connect = self.on_connect
        self.client.on_message = self.on_message

    def start(self):
        self.client.connect(os.environ.get("MQTT_HOST", "192.168.0.32"), int(os.environ.get("MQTT_PORT", "1883")))
        self.client.loop_start()
        if not self.connected.wait(5):
            sys.exit("MQTT connect failed (credentials? see paced_write_test.log)")
        self.client.subscribe([("zigbee2mqtt/bridge/logging", 0), (f"zigbee2mqtt/{DEVICE}", 0)])

    def stop(self):
        self.client.loop_stop()

    def on_connect(self, client, userdata, flags, reason_code, properties):
        self.log.write("mqtt", f"connect: {reason_code}")
        if reason_code == 0:
            self.connected.set()

    def on_message(self, client, userdata, message):
        t = time.perf_counter()
        if message.topic == f"zigbee2mqtt/{DEVICE}":
            self.on_device_state(t, message.payload)
        else:
            self.on_bridge_log(t, message.payload)

    def on_device_state(self, t, payload):
        self.log.write("dev>", payload.decode(errors="replace"))
        try:
            state = json.loads(payload)
            wake_count = state.get("wake_count")
            if state.get("dataset_hash") is not None:
                self.last_dataset_hash = int(state["dataset_hash"])
        except (ValueError, AttributeError):
            wake_count = None
        if wake_count is not None:
            self.wake_changes.append((t, wake_count))
            self.last_wake_count = wake_count

    def on_bridge_log(self, t, payload):
        try:
            entry = json.loads(payload)
        except ValueError:
            return
        text = entry.get("message", "")
        if DEVICE in text or DEVICE_IEEE in text:
            self.log.write(f"z2m:{entry.get('level')}", text)
            if entry.get("level") in ("error", "warning"):
                self.errors.append((t, text))

    def write_attributes(self, attributes):
        """One Z2M raw write (one ZCL frame) with the given {attribute id: (value, type)}."""
        message = {"write": {"cluster": CUSTOM_CLUSTER,
                             "payload": {str(a): {"value": v, "type": t} for a, (v, t) in attributes.items()}}}
        self.log.write("pub", json.dumps(message))
        self.client.publish(f"zigbee2mqtt/{DEVICE}/set", json.dumps(message), qos=1)


# ---- Serial: the device log; survives the port disappearing during a reset ----

class SerialMonitor:
    def __init__(self, log):
        self.log = log
        self.lines = []                     # (t, text)
        self.stopping = threading.Event()
        self.port_closed = threading.Event()  # set while the reader has the port closed (for esptool)
        self.close_port = threading.Event()
        self.thread = threading.Thread(target=self.read_lines, daemon=True)

    def start(self):
        self.thread.start()

    def stop(self):
        self.stopping.set()
        self.thread.join(2)

    @staticmethod
    def open_port():
        port = serial.Serial()
        port.port, port.baudrate, port.timeout, port.dtr, port.rts = PORT, 115200, 0.05, False, False
        port.open()
        return port

    def read_lines(self):
        port = None
        while not self.stopping.is_set():
            if self.close_port.is_set():
                if port:
                    port.close()
                    port = None
                self.port_closed.set()
                time.sleep(0.02)
                continue
            self.port_closed.clear()
            try:
                if port is None:
                    port = self.open_port()
                raw = port.readline()
            except (serial.SerialException, OSError):
                if port:
                    try:
                        port.close()
                    except Exception:
                        pass
                port = None
                time.sleep(0.05)
                continue
            if raw:
                text = raw.decode(errors="replace").rstrip()
                self.lines.append((time.perf_counter(), text))
                self.log.write("ser", text)
        if port:
            port.close()

    def reset_device(self):
        self.close_port.set()
        self.port_closed.wait(5)
        self.log.write("test", "hard reset")
        subprocess.run([IDF_PYTHON, "-m", "esptool", "--chip", "esp32c6", "-p", PORT, "--after", "hard-reset",
                        "chip-id"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
        self.close_port.clear()

    def find(self, text, since):
        """First line from `since` on that contains text: (t, line) or None."""
        return next(((t, line) for t, line in self.lines if t >= since and text in line), None)

    def wait_for_verdict(self, patterns, since, timeout):
        """First line after since that contains one of the (label, text) patterns: (label, seconds) or
        ("timeout", None)."""
        while time.perf_counter() - since < timeout:
            for label, pattern in patterns:
                hit = self.find(pattern, since)
                if hit:
                    return label, hit[0] - since
            time.sleep(0.02)
        return "timeout", None


def wait_for_wake(serial_monitor, z2m, since, settle_s, timeout=30.0):
    """Wait for the HA trigger: Z2M publishes this boot's wake_count, then settle.
    This boot's value is the "wake=N" the device prints at boot; an earlier value is Z2M's cached state."""
    while time.perf_counter() - since < timeout:
        boot = next((line for t, line in serial_monitor.lines if t > since and " wake=" in line), None)
        if boot:
            wake_count = int(boot.rsplit("wake=", 1)[1].split()[0])
            if any(t > since and value == wake_count for t, value in z2m.wake_changes):
                serial_monitor.log.write("test", f"wake counter {wake_count} published by Z2M, settle {settle_s:g} s")
                time.sleep(settle_s)
                return True
        time.sleep(0.02)
    return False


# ---- The test dataset ----

def fit_utf8(text, limit=LINE_MAX_BYTES):
    """The text cut to limit UTF-8 bytes (docs/data-model.md#dataset), never inside a character."""
    return text.encode()[:limit].decode(errors="ignore")


def split_parts(text):
    """A line as ds_entry parts of at most PART_MAX_BYTES bytes each, split at character boundaries."""
    parts = [""]
    for char in fit_utf8(text):
        if len((parts[-1] + char).encode()) > PART_MAX_BYTES:
            parts.append("")
        parts[-1] += char
    assert len(parts) <= LINE_MAX_PARTS, text
    return parts


def dataset_line(index, name, state):
    """Dataset line `index` in the line format II|name|state (docs/data-model.md#line-icons): icon ids from 0x02 (the first real
    icon, 0x21 for line 31), the name cut so the whole line fits LINE_MAX_BYTES bytes."""
    head, tail = f"{index + 2:02X}|", f"|{state}"
    return head + fit_utf8(name, LINE_MAX_BYTES - len(head.encode()) - len(tail.encode())) + tail


def make_dataset(generation, options):
    """(hash, lines) of the test dataset; the generation goes into the texts, so each has its own hash."""
    count = options.dataset_lines
    if options.hungarian_long:
        lines = [dataset_line(i, f"{generation:03d} {i:02d} {HU_TEXTS[i % len(HU_TEXTS)]}", "nyitva") if i % 2 == 0
                 else dataset_line(i, HU_LONG, "nyitva") for i in range(count)]
        lines[0] = dataset_line(0, f"Generáció {generation:03d}", "új")   # the hash changes with every generation
    elif options.hungarian:
        lines = [dataset_line(i, f"{generation:03d} {i:02d} {HU_TEXTS[i % len(HU_TEXTS)]}", "nyitva")
                 for i in range(count)]
    else:
        lines = [dataset_line(i, f"Test {generation:03d} line {i:02d}", "open") for i in range(count)]
    dataset_hash = int(hashlib.sha1("\n".join(lines).encode()).hexdigest()[:6], 16) or 1
    return dataset_hash, lines


def sync_dataset(dataset_hash, lines, options, z2m, serial_monitor):
    """Write the dataset and commit it; returns (verdict, seconds)."""
    start = time.perf_counter()
    dropped = False
    writes = 1   # the commit
    for index, text in enumerate(lines):
        parts = split_parts(text)
        for part_index, part in enumerate(parts):
            more = part_index < len(parts) - 1
            if options.drop_part and not dropped and len(parts) > 1 and not more:
                serial_monitor.log.write("test", f"dropping part {part_index} of line {index}")
                dropped = True
                continue
            line_and_part = index | part_index << 5 | more << 7   # line index, part, "more" (docs/zigbee.md#input-ha-to-device)
            z2m.write_attributes({DS_ENTRY: (f"{dataset_hash:06X}|{line_and_part:02X}|{part}", STRING)})
            writes += 1
            time.sleep(options.spacing_s)
    z2m.write_attributes({DS_COMMIT: ((len(lines) << 24) | dataset_hash, UINT32)})
    # 30 s plus 1.5 s per write: about 0.45 s per write reaches the sleepy device (docs/zigbee.md#dataset-sync), with stalls
    return serial_monitor.wait_for_verdict([("stored", f"ds_commit {dataset_hash:06x} accepted"),
                                            ("already", f"ds_commit {dataset_hash:06x}: already stored"),
                                            ("REJECTED", f"ds_commit {dataset_hash:06x} rejected")],
                                           start, 30 + 1.5 * writes)


# ---- The rounds ----

class Run:
    def __init__(self, options, log, z2m, serial_monitor):
        self.options, self.log, self.z2m, self.serial = options, log, z2m, serial_monitor
        self.results = []         # (verdict, push seconds, dataset verdict or None)
        self.view_id = None
        self.dataset_hash, self.dataset_lines = make_dataset(0, options)
        self.device_dataset = None  # the dataset hash the device has, as far as this run knows

    def reset_and_wait_for_wake(self):
        """Reset the device; the reset time, or None if Z2M did not get the wake report in time."""
        reset_at = time.perf_counter()
        self.serial.reset_device()
        return reset_at if wait_for_wake(self.serial, self.z2m, reset_at, self.options.settle_s) else None

    def ensure_dataset(self, round_number):
        """Send the test dataset unless the device has it; the verdict for the round's line, or None."""
        if self.options.resync_each and round_number > 1:
            self.dataset_hash, self.dataset_lines = make_dataset(round_number, self.options)
        if self.device_dataset is None and self.z2m.last_dataset_hash is not None:
            # Z2M's cached value, as the HA automation uses it (docs/home-assistant.md#push-automation)
            self.device_dataset = self.z2m.last_dataset_hash
        if self.device_dataset == self.dataset_hash:
            return None
        verdict, seconds = sync_dataset(self.dataset_hash, self.dataset_lines, self.options, self.z2m, self.serial)
        self.log.write("test", f"dataset {self.dataset_hash:06x}: {verdict} {seconds}")
        if verdict in ("stored", "already"):
            self.device_dataset = self.dataset_hash
            return f"ds {verdict} {seconds:.2f} s"
        return f"ds {verdict}"

    def measure_awake_time(self, round_number, round_start, dataset_verdict):
        """--no-view: how long the device stays in ACTIVE without a view."""
        active_at = next((t for t, line in self.serial.lines if t > round_start and "REJOIN -> ACTIVE" in line), None)
        time.sleep(self.options.gap_s)
        shutdown_at = next((t for t, line in self.serial.lines if t > round_start and "ACTIVE -> SHUTDOWN" in line),
                           None)
        awake = shutdown_at - active_at if active_at and shutdown_at else None
        self.results.append(("awake", awake, dataset_verdict))
        print(f"round {round_number:2}: no view sent, ACTIVE for {'%.1f s' % awake if awake else 'unknown'}"
              + (f"  [{dataset_verdict}]" if dataset_verdict else ""), flush=True)

    def next_view_id(self):
        if self.options.same_view and self.view_id:
            return self.view_id
        new_id = random.randrange(1, 0xFFFFFF)
        while new_id == self.view_id:
            new_id = random.randrange(1, 0xFFFFFF)
        return new_id

    def push_view(self):
        self.view_id = self.next_view_id()
        bits = random.randrange(0, 1 << self.options.dataset_lines)
        local_now = calendar.timegm(time.localtime())  # local wall-clock seconds since 1970 (docs/data-model.md#view)
        view_dataset = self.dataset_hash ^ 1 if self.options.out_of_sync else self.dataset_hash
        start = time.perf_counter()
        self.z2m.write_attributes({VIEW_BITS: (bits, UINT32), VIEW_DS: (view_dataset, UINT32),
                                   VIEW_TIME: (local_now, UINT32), VIEW_ID: (self.view_id, UINT32)})
        verdict, seconds = self.serial.wait_for_verdict([("accepted", f"view {self.view_id:06x} accepted"),
                                                         ("time", f"view {self.view_id:06x} time only"),
                                                         ("OOS", f"view {self.view_id:06x} out of sync"),
                                                         ("PART", f"view {self.view_id:06x} incomplete")], start, 30)
        if verdict in ("accepted", "time") and seconds >= 5:
            verdict = "LATE " + verdict
        return verdict, seconds, bits

    def run_round(self, round_number):
        wake_seconds = None
        if self.options.reset_each:
            reset_at = self.reset_and_wait_for_wake()
            if reset_at is None:
                print(f"round {round_number:2}: no wake report through Z2M, skipped", flush=True)
                self.results.append(("no wake", None, None))
                time.sleep(self.options.gap_s)
                return
            wake_seconds = time.perf_counter() - reset_at
            round_start = reset_at
        else:
            round_start = time.perf_counter()
        first_error = len(self.z2m.errors)
        self.log.write("test", f"round {round_number}")
        dataset_verdict = self.ensure_dataset(round_number)
        if self.options.no_view:
            self.measure_awake_time(round_number, round_start, dataset_verdict)
            return
        verdict, seconds, bits = self.push_view()
        errors = [text for _, text in self.z2m.errors[first_error:]]
        self.results.append((verdict, seconds, dataset_verdict))
        print(f"round {round_number:2}: view {self.view_id:06X} bits {bits:08X} {verdict:9} "
              f"{'%.2f s' % seconds if seconds is not None else '   -  '}"
              + (f"  [{dataset_verdict}]" if dataset_verdict else "")
              + (f"  (wake counter {self.z2m.last_wake_count} seen {wake_seconds - self.options.settle_s:.1f} s after "
                 f"reset)" if self.options.reset_each else "")
              + (f"  Z2M errors: {len(errors)}, first: {errors[0][:200]}" if errors else ""), flush=True)
        time.sleep(self.options.gap_s)

    def print_summary(self):
        options = self.options
        expected = ("awake",) if options.no_view else ("OOS",) if options.out_of_sync else ("time", "accepted")
        push_times = [s for verdict, s, _ in self.results if s is not None and verdict.split()[-1] in expected]
        late = sum(verdict.startswith("LATE") for verdict, _, _ in self.results)
        print(f"\n{len(push_times)}/{len(self.results)} as expected ({late} of them later than 5 s), "
              f"{options.dataset_lines}-line dataset"
              + (", new dataset every round" if options.resync_each else "")
              + f", {options.spacing_s * 1000:.0f} ms spacing, {options.gap_s:.0f} s gap"
              + (f", reset before every round, settle {options.settle_s:g} s" if options.reset_each
                 else ", device kept awake"), end="")
        print(f"; push time min {min(push_times):.2f} s, avg {sum(push_times) / len(push_times):.2f} s, "
              f"max {max(push_times):.2f} s" if push_times else "")
        print(f"Z2M warnings/errors about the device: {len(self.z2m.errors)}; full log: {self.log.file.name}")


def main():
    options = parse_options(sys.argv[1:])
    log = TestLog(sys.argv[1:])
    z2m = Z2mMonitor(log)
    z2m.start()
    serial_monitor = SerialMonitor(log)
    serial_monitor.start()
    run = Run(options, log, z2m, serial_monitor)
    if not options.reset_each:
        if run.reset_and_wait_for_wake() is None:
            sys.exit("no wake report from the device through Z2M within 30 s")
        print("device awake, Z2M has its report", flush=True)
    for round_number in range(1, options.rounds + 1):
        run.run_round(round_number)
    serial_monitor.stop()
    run.print_summary()
    z2m.stop()


if __name__ == "__main__":
    main()
