"""Dataset + view write test (DESIGN.md 6.5, rev 6; ZIGBEE_PAYLOAD.md 7).

Default mode: reset the device once and wait for its wake report. Then, per round, make sure the device has the
test dataset (DS_LINES entries, SPACING ms apart, then the commit; skipped while the device already has it) and
push one view: the four view_* attributes in one write, random bits, the current local time and a new id. Watch
the device log for the verdict. Rounds are GAP s apart so Z2M's retries of one round never overlap the next.

--reset-each: also reset the device before every round (the real wake path: every wake is a reboot + rejoin),
and only publish after Z2M has received the wake report, as the HA automation does.

Env: MQTT_HOST (default 192.168.0.32), MQTT_PORT (1883), MQTT_USER, MQTT_PASS.
--settle=S: after the wake trigger, wait S seconds before the first write (default 0.5).
The wake trigger is a change of the wake counter (analog_input_2) in the device's Z2M state, as in the HA
automation (DESIGN.md 7), not the serial log.

--ds-lines=N: entries in the test dataset, 1 to 32 (default 8).
--resync-each: a new dataset (new texts, new hash) in every round, then the view: tests the multi-frame path.
--oos: views name a dataset the device does not have; expect "out of sync" and nothing drawn.
--same: the view id stays the same after round 1; expect "time only" (the footer time updates).

Usage: python paced_write_test.py ROUNDS [SPACING_MS] [GAP_S] [--reset-each] [--settle=S] [--ds-lines=N]
       [--resync-each] [--oos] [--same]
(The rev 5 snapshot mode, --lines=N, is in git history before the rev 6 firmware.)
Full serial and Z2M logs go to paced_write_test_<date>_<time>.log in LOG_DIR (default %TEMP%/gl_test).
"""
import calendar, hashlib, json, os, random, subprocess, sys, threading, time

import paho.mqtt.client as mqtt
import serial

PORT = "COM5"
IDF_PY = r"C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe"
DEVICE = "gf_lobby_greenlight"
CLUSTER = 64650

args = [a for a in sys.argv[1:] if not a.startswith("--")]
reset_each = "--reset-each" in sys.argv
settle = next((float(a.split("=", 1)[1]) for a in sys.argv[1:] if a.startswith("--settle=")), 0.5)
ds_lines = next((int(a.split("=", 1)[1]) for a in sys.argv[1:] if a.startswith("--ds-lines=")), 8)
if not 1 <= ds_lines <= 32:
    sys.exit("--ds-lines must be 1 to 32")
resync_each = "--resync-each" in sys.argv
oos = "--oos" in sys.argv
same = "--same" in sys.argv
rounds = int(args[0]) if len(args) > 0 else 10
spacing = (int(args[1]) if len(args) > 1 else 100) / 1000.0
gap = float(args[2]) if len(args) > 2 else 15.0

log_dir = os.environ.get("LOG_DIR", os.path.join(os.environ.get("TEMP", "."), "gl_test"))
os.makedirs(log_dir, exist_ok=True)
logf = open(os.path.join(log_dir, time.strftime("paced_write_test_%Y%m%d_%H%M%S.log")), "w", encoding="utf-8")
T0 = time.perf_counter()
logf.write(f"# args: {' '.join(sys.argv[1:])}\n")


def log(src, text):
    logf.write(f"{time.perf_counter() - T0:9.3f} {src} {text}\n")
    logf.flush()


# ---- MQTT: Z2M log lines about the device, and the device's own state messages ----
z2m_events = []      # (t, message) for warnings/errors about the device
device_msgs = []     # t of every state message the device published through Z2M
wake_changes = []    # (t, value) of the wake counter (analog_input_2) in every device message
last_wake = [None]
cli = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
if os.environ.get("MQTT_USER"):
    cli.username_pw_set(os.environ["MQTT_USER"], os.environ.get("MQTT_PASS"))
connected = threading.Event()
cli.on_connect = lambda c, u, f, rc, p: (log("mqtt", f"connect: {rc}"), rc == 0 and connected.set())


def on_msg(c, u, m):
    t = time.perf_counter()
    if m.topic == f"zigbee2mqtt/{DEVICE}":
        device_msgs.append(t)
        log("dev>", m.payload.decode(errors="replace"))
        try:
            w = json.loads(m.payload).get("analog_input_2")
        except ValueError:
            w = None
        if w is not None:
            wake_changes.append((t, w))
            last_wake[0] = w
        return
    try:
        d = json.loads(m.payload)
    except ValueError:
        return
    msg = d.get("message", "")
    if DEVICE in msg or "0x10bda3fffea02724" in msg:
        log(f"z2m:{d.get('level')}", msg)
        if d.get("level") in ("error", "warning"):
            z2m_events.append((t, msg))


cli.on_message = on_msg
cli.connect(os.environ.get("MQTT_HOST", "192.168.0.32"), int(os.environ.get("MQTT_PORT", "1883")))
cli.loop_start()
if not connected.wait(5):
    sys.exit("MQTT connect failed (credentials? see paced_write_test.log)")
cli.subscribe([("zigbee2mqtt/bridge/logging", 0), (f"zigbee2mqtt/{DEVICE}", 0)])


def publish(attrs):
    """One Z2M raw write (one ZCL frame) with the given {attribute id: (value, type)}."""
    msg = {"write": {"cluster": CLUSTER, "payload": {str(a): {"value": v, "type": t} for a, (v, t) in attrs.items()}}}
    log("pub", json.dumps(msg))
    cli.publish(f"zigbee2mqtt/{DEVICE}/set", json.dumps(msg), qos=1)


VIEW_BITS, VIEW_DS, VIEW_TIME, VIEW_ID, DS_ENTRY, DS_COMMIT = 0x20, 0x21, 0x22, 0x23, 0x30, 0x31
U32, STR = 35, 66


def make_dataset(gen):
    texts = [f"Test {gen:03d} line {i:02d}" for i in range(ds_lines)]
    h = int(hashlib.sha1("\n".join(texts).encode()).hexdigest()[:6], 16) or 1
    return h, texts


# ---- serial: survives the port disappearing during a reset ----
serial_lines = []    # (t, text)
stop = threading.Event()


def open_serial():
    s = serial.Serial()
    s.port, s.baudrate, s.timeout, s.dtr, s.rts = PORT, 115200, 0.05, False, False
    s.open()
    return s


port_free = threading.Event()   # set while the reader has the port closed (for esptool)
want_free = threading.Event()


def reader():
    s = None
    while not stop.is_set():
        if want_free.is_set():
            if s:
                s.close()
                s = None
            port_free.set()
            time.sleep(0.02)
            continue
        port_free.clear()
        try:
            if s is None:
                s = open_serial()
            ln = s.readline()
        except (serial.SerialException, OSError):
            if s:
                try:
                    s.close()
                except Exception:
                    pass
            s = None
            time.sleep(0.05)
            continue
        if ln:
            text = ln.decode(errors="replace").rstrip()
            serial_lines.append((time.perf_counter(), text))
            log("ser", text)
    if s:
        s.close()


th = threading.Thread(target=reader, daemon=True)
th.start()


def reset_device():
    want_free.set()
    port_free.wait(5)
    log("test", "hard reset")
    subprocess.run([IDF_PY, "-m", "esptool", "--chip", "esp32c6", "-p", PORT, "--after", "hard-reset", "chip-id"],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
    want_free.clear()


def wait_for_wake(t_reset, timeout=30.0):
    """Wait for the HA trigger: Z2M publishes this boot's wake counter (analog_input_2), then settle.
    This boot's value is the "wake=N" the device prints at boot; an earlier value is Z2M's cached state."""
    while time.perf_counter() - t_reset < timeout:
        boot = next((l for t, l in serial_lines if t > t_reset and " wake=" in l), None)
        if boot:
            n = int(boot.rsplit("wake=", 1)[1].split()[0])
            hit = next((t for t, w in wake_changes if t > t_reset and w == n), None)
            if hit:
                log("test", f"wake counter {n} published by Z2M, settle {settle:g} s")
                time.sleep(settle)
                return True
        time.sleep(0.02)
    return False


def find(substr, since):
    return next(((t, l) for t, l in serial_lines if t >= since and substr in l), None)


def wait_verdict(patterns, since, timeout):
    """First serial line after since that contains one of the patterns: (label, seconds) or ("timeout", None)."""
    while time.perf_counter() - since < timeout:
        for label, pat in patterns:
            hit = find(pat, since)
            if hit:
                return label, hit[0] - since
        time.sleep(0.02)
    return "timeout", None


def sync_dataset(h, texts):
    """Write the dataset and commit it; returns (verdict, seconds)."""
    t = time.perf_counter()
    for i, text in enumerate(texts):
        publish({DS_ENTRY: (f"{h:06X}|{i:02X}|{text}", STR)})
        time.sleep(spacing)
    publish({DS_COMMIT: ((len(texts) << 24) | h, U32)})
    return wait_verdict([("stored", f"ds_commit {h:06x} accepted"), ("already", f"ds_commit {h:06x}: already stored"),
                         ("REJECTED", f"ds_commit {h:06x} rejected")], t, 30)


results = []   # (verdict, push seconds, dataset verdict or None)
view_id = None
ds_hash, ds_texts = make_dataset(0)
device_ds = None   # the dataset hash the device has, as far as this run knows
if not reset_each:
    t_r = time.perf_counter()
    reset_device()
    if not wait_for_wake(t_r):
        sys.exit("no wake report from the device through Z2M within 30 s")
    print("device awake, Z2M has its report", flush=True)

for r in range(1, rounds + 1):
    if reset_each:
        t_r = time.perf_counter()
        reset_device()
        if not wait_for_wake(t_r):
            print(f"round {r:2}: no wake report through Z2M, skipped", flush=True)
            results.append(("no wake", None, None))
            time.sleep(gap)
            continue
        wake_s = time.perf_counter() - t_r

    n_err = len(z2m_events)
    log("test", f"round {r}")
    ds_verdict = None
    if resync_each and r > 1:
        ds_hash, ds_texts = make_dataset(r)
    if device_ds != ds_hash:
        ds_verdict, ds_s = sync_dataset(ds_hash, ds_texts)
        log("test", f"dataset {ds_hash:06x}: {ds_verdict} {ds_s}")
        if ds_verdict in ("stored", "already"):
            device_ds = ds_hash
            ds_verdict = f"ds {ds_verdict} {ds_s:.2f} s"
        else:
            ds_verdict = f"ds {ds_verdict}"

    if not (same and view_id):
        new = random.randrange(1, 0xFFFFFF)
        while new == view_id:
            new = random.randrange(1, 0xFFFFFF)
        view_id = new
    bits = random.randrange(0, 1 << ds_lines)
    local_now = calendar.timegm(time.localtime())  # local wall-clock seconds since 1970 (DESIGN.md 5.2)
    view_ds = ds_hash ^ 1 if oos else ds_hash
    t_start = time.perf_counter()
    publish({VIEW_BITS: (bits, U32), VIEW_DS: (view_ds, U32), VIEW_TIME: (local_now, U32), VIEW_ID: (view_id, U32)})
    verdict, dur = wait_verdict([("accepted", f"view {view_id:06x} accepted"), ("time", f"view {view_id:06x} time only"),
                                 ("OOS", f"view {view_id:06x} out of sync"), ("PART", f"view {view_id:06x} incomplete")],
                                t_start, 30)
    if verdict in ("accepted", "time") and dur >= 5:
        verdict = "LATE " + verdict
    errs = [m for _, m in z2m_events[n_err:]]
    results.append((verdict, dur, ds_verdict))
    print(f"round {r:2}: view {view_id:06X} bits {bits:08X} {verdict:9} {'%.2f s' % dur if dur is not None else '   -  '}"
          + (f"  [{ds_verdict}]" if ds_verdict else "")
          + (f"  (wake counter {last_wake[0]} seen {wake_s - settle:.1f} s after reset)" if reset_each else "")
          + (f"  Z2M errors: {len(errs)}, first: {errs[0][:200]}" if errs else ""), flush=True)
    time.sleep(gap)

stop.set()
th.join(2)
expect = ("OOS",) if oos else ("time", "accepted")
ok = [d for v, d, _ in results if d is not None and v.split()[-1] in expect]
late = sum(v.startswith("LATE") for v, d, _ in results)
print(f"\n{len(ok)}/{len(results)} as expected ({late} of them later than 5 s), {ds_lines}-line dataset"
      + (", new dataset every round" if resync_each else "") + f", {spacing * 1000:.0f} ms spacing, {gap:.0f} s gap"
      + (f", reset before every round, settle {settle:g} s" if reset_each else ", device kept awake"), end="")
print(f"; push time min {min(ok):.2f} s, avg {sum(ok) / len(ok):.2f} s, max {max(ok):.2f} s" if ok else "")
print(f"Z2M warnings/errors about the device: {len(z2m_events)}; full log: {logf.name}")
cli.loop_stop()
