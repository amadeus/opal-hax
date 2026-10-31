# Opal C1 tools

Reverse-engineered tooling for the Opal C1 webcam (Intel Movidius Myriad X + Sony IMX378,
Opal.app 0.26 / build 25.992, Luxonis depthai-core 2.15.1 "audioproc" fork). All tools link
against Opal.app's own `libdepthai-core.dylib`, so they speak exactly the protocol of Opal's
private device firmware (`f57b4691…`).

## What was changed on the camera (2026-10-31)

| | Before | After |
|---|---|---|
| Bootloader (flash) | 0.0.4 ("VSC Loopback Device") | 0.0.15 (`bb9e4c86…`, the one bundled in Opal.app) |
| Standalone webcam program | old factory/legacy build, USB PID `f63b`, sensor always on | Opal's pipeline + idle watcher, USB PID `f63d` |

The idle watcher is a Script node on the camera (`standalone.hpp`) that reads GPIO 58 (driven by
the UVC node: 1 = a webcam client is streaming) and sends `setStopStreaming` 3 s after the last
client leaves and `setStartStreaming` when one connects. The output is otherwise identical to Opal's
flashed pipeline: 4K sensor, ISP 1/2 → 1080p UVC, 180° rotation, AF auto, 60 Hz anti-banding,
mic +28 dB via UAC.

## Measurements (host-booted, IMX378 tuning `Opal_v0.12.bin`, 4K30)

- Streaming: chip 46.8 → 49.4 °C over 5 min (still rising), camera CPU ~27 %
- Sensor stopped: 42.4 → 37.3 °C over 5 min (still falling), CPU ~2 %
- Restart: 30 fps again within < 5 s; auto-exposure settles in ~0.5 s
- `thermal_run2.log`, `idle3.log` have the raw data

## Commands

```bash
./fetch-deps.sh                      # once: fetch matching public headers into vendor/
./build.sh opaltool.cpp bin/opaltool # rebuild

# Quit the Opal app first (it grabs the camera whenever it reboots):
launchctl bootout gui/$(id -u)/com.opalcamera.OpalCamera.startOnUsbPlugged.agent
osascript -e 'tell application "Opal" to quit'

bin/opaltool hostboot idle 60   # RAM-only test of the idle-watcher pipeline, prints temps + camera logs
bin/opaltool hostboot orig 60   # RAM-only test of Opal's original pipeline
bin/opaltool flash idle         # flash the idle-watcher webcam program (current)
bin/opaltool flash orig         # restore Opal's original webcam program (sensor always on)
bin/opaltool blversion          # print flashed bootloader version
bin/probe                       # passive: list depthai-visible devices and their state

# Restore the Opal app afterwards:
launchctl bootstrap gui/$(id -u) /Library/LaunchAgents/com.opalcamera.OpalCamera.startOnUsbPlugged.agent.plist
open -a Opal
```

`thermal` (`./build.sh thermal.cpp bin/thermal`) runs a streaming/stopped/restarted temperature
experiment: `bin/thermal 300 300 30`.

## Key findings (see conversation for evidence)

- Opal.app only adds Sentry breadcrumbs at avg chip temp > 65 °C ("Painful temperature!") and
  FPS < 30; nothing throttles.
- In app mode the app stops the sensor only when no meeting app is connected **and** its
  popover is closed (`StreamingState: appsConnectedToPlugin / isPopoverOpen / CamEx`).
- The app hard-resets the camera on every start/teardown (`0xF5/0x0DA1` vendor request for `f63b`
  devices; `DeviceBase::close()` otherwise). With bootloader 0.0.4 every host boot also went through
  the Myriad USB ROM (`2485`); with 0.0.15 it boots directly.
- Opal's firmware intermittently hit `Fatal error on MSS CPU: trap: 00, address: 00000000`
  ~2 s after streaming start (2 of ~15 boots in testing), the leading suspect for hangs.
- The firmware contains a thermal-shutdown path ("Temperature above {}C. Device will shutdown").
- Host-booting a UVC pipeline needs `BoardConfig.uvcEnable = true`; on-camera `GPIO.read(58)`
  needs a non-exclusive `GPIO.setup(58, GPIO.OUT, GPIO.PULL_NONE, False)` first (else returns 9).

## App-side tools (2026-10-31)

- `opalspy/` — `opalspy.dylib`, injected into Opal (`open -a Opal --env DYLD_INSERT_LIBRARIES=$PWD/opalspy/opalspy.dylib`):
  camera telemetry (`~/Library/Logs/opal-watch/telemetry.log`), settings dump (`opal-settings.txt`) and the
  streaming debounce (`debounce.log`). Build:
  `clang -arch arm64 -dynamiclib -fobjc-arc -framework AppKit -framework CoreMediaIO -o opalspy/opalspy.dylib opalspy/opalspy.m opalspy/debounce.m opalspy/cmio_inuse.m && codesign -s - -f opalspy/opalspy.dylib`
  `cmio_inuse` (standalone) prints whether any app is using the Opal C1 virtual camera. `cpu-sampler.sh` logs Opal CPU.
- `opal-launch/` — launcher that starts Opal with opalspy at login and on camera attach. Install with
  `opal-launch/install.sh` (revert: `install.sh uninstall`).
