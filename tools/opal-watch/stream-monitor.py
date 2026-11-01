#!/usr/bin/python3
# stream-monitor.py [summary_secs]: one line per noteworthy Opal event + periodic temp/load summary; auto-runs
# opal-capture-state on crash signatures (10-min cooldown). Also appends to ~/Library/Logs/opal-watch/overnight-<date>.log.
import os, re, time, glob, subprocess, sys
L = os.path.expanduser("~/Library/Logs/opal-watch/")
SUMMARY_EVERY = int(sys.argv[1]) if len(sys.argv) > 1 else 120
REPO = "/Users/amadeus/Developer/experiments/opal-camera"
NIGHT = open(L + "overnight-" + time.strftime("%Y-%m-%d") + ".log", "a")
CRASH = re.compile(r"terminateDevice: destroying 0x03e7/f63b|Fatal|[Tt]emperature above|Device will shutdown|watchdog|"
                   r"Communication exception|X_LINK_ERROR|Couldn't read data|cameraNotFound|No device available|crash", re.I)
last_capture = 0
files = {"cam": L + "camera.log", "tel": L + "telemetry.log", "watch": L + time.strftime("%Y-%m-%d") + ".log"}
pos = {k: os.path.getsize(v) if os.path.exists(v) else 0 for k, v in files.items()}
EVENT = re.compile(r"terminateDevice|enumerated 0x03e7|StreamingState|Low FPS|Painful|[Tt]emperature above|Fatal|crash|watchdog|"
                   r"Device (was )?disconnected|cameraNotFound|No device available|Resetting|UVC device reset|XLink.*(error|fail)|"
                   r"Communication exception|X_LINK|timeout|Couldn't read|stopStream|startStream|ended\(pid", re.I)
NOISE = re.compile(r"orientative|Inference thread count|openGated|Audio:|pipe stalled", re.I)
last_sum = 0; temps = []; cpu = (0, 0); ddr = 0; fps = None; gain = None; peak = 0
start = time.time(); last_tel = 1; last_n = 0; last_tel_change = time.time(); stale_warned = False
def out(s):
    global last_capture
    line = time.strftime("%H:%M:%S") + " " + s
    print(line, flush=True); NIGHT.write(line + "\n"); NIGHT.flush()
    if CRASH.search(s) and time.time() - last_capture > 600:
        last_capture = time.time()
        subprocess.Popen([REPO + "/tools/opal-watch/opal-capture-state", "auto: " + s[:120]],
                         stdout=NIGHT, stderr=NIGHT)
        print(time.strftime("%H:%M:%S") + " CAPTURED incident state (opal-capture-state)", flush=True)
while True:
    for k, p in files.items():
        try:
            sz = os.path.getsize(p)
        except OSError:
            continue
        if sz < pos[k]: pos[k] = 0
        if sz == pos[k]: continue
        with open(p, errors="replace") as f:
            f.seek(pos[k]); data = f.read(); pos[k] = f.tell()
        for line in data.splitlines():
            if k == "tel":
                m = re.search(r"raw=\[([\d.]+) [\d.]+ \| [\d.]+ [\d.]+ \| [\d.]+ [\d.]+ \| [\d.]+ [\d.]+ \| ([\d.]+) ([\d.]+) ([\d.]+)\]", line)
                if m:
                    ddr = float(m.group(1))
                continue
            if k == "cam":
                m = re.search(r"\[telemetry\] chip avg=([\d.]+)C.*cpu css=(\d+)% mss=(\d+)%", line)
                if m:
                    t = float(m.group(1)); temps.append(t); peak = max(peak, t); cpu = (float(m.group(2)), float(m.group(3)))
                    if t >= 65: out(f"HOT: chip {t:.1f} C")
                    continue
                m = re.search(r"fps:([\d.]+).*totalgain:\s*([\d.]+)x.*temp:(\d+)", line)
                if m:
                    fps = float(m.group(1)); gain = float(m.group(2))
                    if fps < 25: out(f"LOW FPS from sensor: {line[24:150]}")
                    continue
                if NOISE.search(line): continue
                if re.search(r"WARN|ERR|error|crit|fatal", line, re.I) or EVENT.search(line):
                    out("camera.log: " + line[24:220])
                continue
            if NOISE.search(line): continue
            if EVENT.search(line):
                out("watch: " + re.sub(r"\[[^\]]*\] \[[^\]]*\] ", "", line[20:])[:220])
    now = time.time()
    if now - last_sum >= SUMMARY_EVERY and temps:
        last_sum = now
        out(f"SUMMARY +{(now-start)/60:.0f}min: chip {temps[-1]:.1f} C (peak {peak:.1f}), CSS {cpu[0]:.0f}% MSS {cpu[1]:.0f}%, "
            f"DDR {ddr:.0f} MB, sensor fps {fps}, gain {gain}x")
    if temps and now - last_tel > 0 and len(temps) == last_n and now - last_tel_change > 120 and not stale_warned:
        stale_warned = True; out("NOTE: no camera telemetry for 2 min (camera silent or app not connected)")
    if len(temps) != last_n:
        last_n = len(temps); last_tel_change = now; stale_warned = False
    time.sleep(2)
