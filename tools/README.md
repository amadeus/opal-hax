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

## Camera profiler (2026-11-05): `tools/profile/profile.cpp`

What the camera itself costs, piece by piece. Runs Opal's exact app-mode pipeline (the `tools/win` template) on the
camera from this Mac with parts switched off, rate-limited or restructured, and records every second: LEON CSS/MSS CPU,
the four chip temperature sensors (CSS, MSS, UPA = SHAVE/NN cluster, DSS), DDR/CMX/heap, and the message rate of every
output stream. There is no finer profiler in the firmware: the RPC list is `getLeonCss/MssCpuUsage`, `getChipTemperature`,
`getDdr/CmxUsage`, heap usage, log level (RTEMS' per-thread CPU report is compiled in but nothing triggers it), so
ablation is the way to attribute load. The four temperature sensors track each other within 1 °C (small die), so they
don't localize heat either.

```bash
# build (part of the smoke CMake project; needs tools/win/build-depthai.sh + make-template.py done once)
CMAKE_POLICY_VERSION_MINIMUM=3.5 cmake --build tools/vendor/smoke-build-mac --target profile
# take the camera from Opal first (custom build labels):
launchctl bootout gui/$(id -u)/com.amadeus.oc.OpalCamera.startOnUsbPlugged.agent; osascript -e 'tell application "Opal" to quit'
tools/vendor/smoke-build-mac/profile tools/vendor/opal-template 60                  # Opal defaults: face + segmentation on
tools/vendor/smoke-build-mac/profile tools/vendor/opal-template 60 choke=1          # what Opal actually sends (see below)
tools/vendor/smoke-build-mac/profile tools/vendor/opal-template 60 face=0 seg=0     # no networks
tools/vendor/smoke-build-mac/profile tools/vendor/opal-template 60 sensor=1080p dump=/tmp/x   # on-sensor binning + a frame
tools/vendor/smoke-build-mac/profile tools/vendor/opal-template 60 gate=face:3      # face NN every 3rd frame (on-camera Script)
# restore:
launchctl bootstrap gui/$(id -u) /Library/LaunchAgents/com.amadeus.oc.OpalCamera.startOnUsbPlugged.agent.plist; open -g -a Opal
```

Options: `res=4k|1440|1080|720`, `sensor=1080p|4k|12mp`, `fps=`, `preview=WxH`, `face= seg= hands= choke=`, `gate=face:N,seg:N,pd:N,lm:N`
(`gateproc=css|mss`), `rm=<node ids>` (structural), `stream=0`, `log=debug`, `dump=<prefix>` (NV12 at t=10 s; `ffmpeg -f rawvideo
-pix_fmt nv12 -s 1920x1080 -i x.nv12 x.png`), `csv=`. Output: `~/Library/Logs/opal-watch/profile/<name>-<time>.csv` + `summary.log`.
A camera still running Opal's firmware (03e7:f63b) is UVC-reset first. zsh does not word-split `$v`: pass the options as
separate words, not through a variable.

Results 2026-11-05 (1080p, 30 fps, room ~22 °C, chip 44–49 °C during the 40 s runs; CPU = LEON average over the last third):

| pipeline state | CSS | MSS | notes |
|---|---|---|---|
| sensor + ISP + XLink video, no networks | 25 % | 6 % | base: ISP/sensor/USB; temperature still climbs ~3 °C/min from 45 °C |
| + segmentation (bokeh mask, 256×144) | 27 % | 18 % | 26 inferences/s |
| + face detection (300×300) + segmentation | 30 % | 28 % | Opal with focus lock + bokeh, gestures off: 24 + 26 inferences/s |
| the same, but the hand-pose manager Script un-choked (what Opal sends at every stream start) | 48 % | 65 % | 3 hand-pose NNs run at full rate, face/seg drop to ~15/s |
| no networks + un-choked | 45 % | 38 % | the hand-pose chain alone |
| face + seg + gestures fully on | 55 % | 73 % | same as the un-choked case + the host asking for them |
| face + seg, face NN fed every 3rd frame by an on-camera gate Script (`gate=face:3`) | 30 % | 21 % | face 8.8/s but segmentation also dropped 26 → 18/s (the Script on CSS contends): not a clean win |
| no networks, sensor in its 1080p (2×2 binned) mode instead of 4K + ISP ½ | 27 % | 8 % | DDR 59 vs 81 MB, 30.1 fps to the host vs 26.4, slower warm-up; picture visibly softer at night (ISO 2400) |

8-minute runs (same room, B started from A's heat): A = as Opal sends it (un-choked): CSS 50 % / MSS 64 %, 52.5 → 58.3 °C,
still +0.8 °C/min at the end; B = choke guard: CSS 30 % / MSS 28 %, 57.4 → 59.6 °C, +0.5 °C/min. A clean cold-start thermal
comparison is still to do; the app history (29/28 sessions plateau ~57 °C vs 55/70 sessions 60–69 °C) says several °C.

4K over the link (2026-11-05 23:27, Opal's networks on, gestures choked; `enc=h264:60000` etc. puts a VideoEncoder between
ColorCamera.video and the rgb XLinkOut; latency = frame capture timestamp → arrival on the Mac):

| 4K variant | CSS | MSS | to the Mac | capture → host | per frame | chip after 40–60 s |
|---|---|---|---|---|---|---|
| raw NV12 (what Opal does) | 41 % | 41 % | 23.9 fps | 229 ms | 12.2 MB | 59 °C, +7.7 °C/min |
| H.264 Main, 60 Mb/s, no B-frames | 59 % | 72 % | 26.4 fps | 90 ms | 244 KB | 64 °C, +6.7 °C/min |
| H.265 Main, 40 Mb/s | 54 % | 70 % | 26.5 fps | 96 ms | 163 KB | 65 °C, +9 °C/min |
| 1080p raw (reference) | 30 % | 28 % | 26.5 fps | 54 ms | 3.0 MB | 59 °C, +5.2 °C/min |

**Frame rate (2026-11-06): the 26.4 fps of every XLink variant is Opal's multi-command camera controls.** The sensor runs
30.1; frames go missing on the camera (sequence gaps, 66 ms holes) once a camera control message applies two or more 3A
commands at once (Opal's stream start carries 8, its pipeline's initial control 8 more), for the rest of the session. One
command per message, 40 ms apart: 30.0 fps, no gaps; back-to-back singles: 26.6. Found by bisecting on the UVC output
(the flashed program's pipeline, which sends single commands, does 30.2 distinct frames/s), confirmed over XLink with
`profile … init.cmdMask=0 bare=1 seq=ae,awb,scene,exp,ctrl,sat` (30.4, 0 gaps). Ruled out: ISP throughput, USB, XLink
queues/streams/chunking, rotation, preview, video size, tuning blob, dark-room auto exposure, AF mode. Fixed in
`tools/opalcore/fpsfix.mm` (29.9 distinct fps through Opal's virtual camera, was 26.4). Profiler knobs used:
`src= rgbq= rgbmeta= manip= uvc= pipe=manual man.* cam.<prop>= init.<field>= bare= add= seq= seqdelay= inq= noinq= blonly=`.

So the camera's USB 3.0 link (5 Gb/s negotiated; the dock hub is 10 Gb/s) caps raw 4K at ~24 fps with 230 ms of queueing,
and encoding does get 4K through it at full rate with 90 ms latency, but the encoder costs the camera about as much as the
hand-pose bug did (+18 % CSS, +31 % MSS, face/segmentation inferences 22 → 15/s) and sends it straight into the hot regime.
Decoding the captured stream on this Mac (ffmpeg): software 210 fps = 4.7 ms per 4K frame (multi-core), VideoToolbox hwaccel
path 60 fps = 16 ms per frame including the copy back from GPU memory (not a clean per-frame latency number). The 26.4 fps ceiling of every
XLink variant (raw 1080p too) is on the camera's XLink side, not bandwidth.

So Opal's "high load" sessions (CSS 55 % / MSS 70 %, 60–69 °C in the Meet call and the overnight soak) are the hand-pose
pipeline running with every gesture off: `CAMERA:: Setting choke on: 1` at stream start un-chokes the on-camera manager
Script, which then re-enables the palm-detection ImageManip itself; the host's `skipCurrentImage`/`processInputs=false`
don't stop it. Fix: opalcore's gesture choke guard (`[choke]` lines in camera.log).
