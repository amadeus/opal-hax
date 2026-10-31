#!/usr/bin/env python3
"""opal-watch: background recorder for Opal C1 camera events.

Writes ~/Library/Logs/opal-watch/YYYY-MM-DD.log containing:
  * LOG   lines: macOS unified-log events for the camera (kernel USB attach/detach of 03e7 devices,
                 the CalDigit dock), the Opal app / device service / video service / camera extension
                 (subsystem com.opalcamera.*, debug level), and UVCAssistant mentions of Opal.
  * USB   lines: snapshot of every Movidius (VID 0x03e7) USB device: PID, name, speed. Written when it
                 changes, and as a heartbeat every 30 minutes.
  * PROC  lines: which Opal processes are running (on change).
  * CRASH lines: new crash reports in ~/Library/Logs/DiagnosticReports mentioning Opal.
Keeps 30 days of files. Run by launchd (see local.opal-watch.plist).
"""
import datetime
import glob
import os
import re
import subprocess
import threading
import time

LOG_DIR = os.path.expanduser("~/Library/Logs/opal-watch")
RETENTION_DAYS = 30
SNAPSHOT_EVERY_S = 60
HEARTBEAT_EVERY_S = 30 * 60

PREDICATE = (
    '(process == "kernel" AND (eventMessage CONTAINS "0x03e7" OR eventMessage CONTAINS "Opal C1"'
    ' OR eventMessage CONTAINS "Luxonis" OR eventMessage CONTAINS "VSC Loopback"'
    ' OR eventMessage CONTAINS "Movidius" OR eventMessage CONTAINS "TS5 Plus"))'
    ' OR (subsystem BEGINSWITH "com.opalcamera")'
    ' OR (process == "UVCAssistant" AND eventMessage CONTAINS "Opal")'
)

_lock = threading.Lock()


def write(kind, text):
    now = datetime.datetime.now()
    path = os.path.join(LOG_DIR, now.strftime("%Y-%m-%d") + ".log")
    with _lock, open(path, "a") as f:
        f.write(f"{now:%Y-%m-%d %H:%M:%S} {kind:5} {text.rstrip()}\n")


def prune():
    cutoff = time.time() - RETENTION_DAYS * 86400
    for p in glob.glob(os.path.join(LOG_DIR, "*.log")):
        if os.path.getmtime(p) < cutoff:
            os.remove(p)


def log_stream_loop():
    while True:
        try:
            proc = subprocess.Popen(
                ["/usr/bin/log", "stream", "--level", "debug", "--style", "compact", "--predicate", PREDICATE],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace")
            write("META", "log stream started")
            for line in proc.stdout:
                if line.startswith("Filtering the log data") or line.startswith("Timestamp"):
                    continue
                # strip the log's own timestamp prefix; ours is added by write()
                write("LOG", line[24:] if re.match(r"\d{4}-\d\d-\d\d ", line) else line)
            write("META", f"log stream exited rc={proc.wait()}")
        except Exception as e:  # keep recording no matter what
            write("META", f"log stream error: {e!r}")
        time.sleep(5)


def usb_snapshot():
    out = subprocess.run(["/usr/sbin/ioreg", "-p", "IOUSB", "-l", "-w0"], capture_output=True, text=True).stdout
    devs = []
    for block in re.split(r"\n(?=[ |]*\+-o )", out):
        if re.search(r'"idVendor" = 999\b', block):
            pid = re.search(r'"idProduct" = (\d+)', block)
            name = re.search(r'"USB Product Name" = "([^"]*)"', block)
            speed = re.search(r'"UsbLinkSpeed" = (\d+)', block)
            loc = re.search(r'"locationID" = (\d+)', block)
            devs.append("pid=%04x name=%s speed=%sMbps loc=%08x" % (
                int(pid.group(1)) if pid else 0, name.group(1) if name else "?",
                int(speed.group(1)) // 1_000_000 if speed else "?", int(loc.group(1)) if loc else 0))
    return "; ".join(sorted(devs)) or "NO MOVIDIUS DEVICE PRESENT"


def proc_snapshot():
    out = subprocess.run(["/bin/ps", "-axo", "pid=,comm="], capture_output=True, text=True).stdout
    names = []
    for line in out.splitlines():
        pid, _, comm = line.strip().partition(" ")
        base = os.path.basename(comm)
        if "Opal.app/Contents/MacOS/Opal" in comm or base in ("OpalCameraDeviceService", "OpalCameraVideoService"):
            names.append(f"{base}({pid})")
    return " ".join(sorted(names)) or "no Opal app processes"


def opal_crash_reports():
    # case-insensitive FS: *Opal* also matches our own 'opaltool' reports, so exclude those
    return {p for p in glob.glob(os.path.expanduser("~/Library/Logs/DiagnosticReports/*Opal*"))
            if not os.path.basename(p).lower().startswith("opaltool")}


def snapshot_loop():
    last_usb = last_proc = None
    last_hb = 0.0
    seen_crashes = opal_crash_reports()
    while True:
        try:
            usb, procs = usb_snapshot(), proc_snapshot()
            now = time.time()
            if usb != last_usb or now - last_hb > HEARTBEAT_EVERY_S:
                write("USB", usb)
            if procs != last_proc or now - last_hb > HEARTBEAT_EVERY_S:
                write("PROC", procs)
            if now - last_hb > HEARTBEAT_EVERY_S:
                last_hb = now
                prune()
            last_usb, last_proc = usb, procs
            crashes = opal_crash_reports()
            for c in sorted(crashes - seen_crashes):
                write("CRASH", c)
            seen_crashes = crashes
        except Exception as e:
            write("META", f"snapshot error: {e!r}")
        time.sleep(SNAPSHOT_EVERY_S)


def main():
    os.makedirs(LOG_DIR, exist_ok=True)
    write("META", f"opal-watch started (pid {os.getpid()})")
    threading.Thread(target=log_stream_loop, daemon=True).start()
    snapshot_loop()


if __name__ == "__main__":
    main()
