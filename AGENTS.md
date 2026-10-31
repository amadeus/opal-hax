# Opal C1 camera — investigation context & hang playbook

You are picking up an ongoing investigation into an **Opal C1 webcam** that, over time, gets hot and
then "gets stuck": it disappears as a camera option, the Opal menu-bar icon blinks (the app is
searching for it), and only unplug/replug fixes it. The manufacturer has abandoned it. The owner is
happy for us to reverse-engineer the app, run tools against the camera, and flash firmware, but
**ask before any flash operation** and never re-flash the bootloader (already done; see below).

If the user says the camera hung or crashed, go straight to **Hang playbook** below.

## What the hardware/software is (established 2026-10-31)

- Intel Movidius **Myriad X** (USB VID `0x03e7`) + **Sony IMX378** sensor (firmware log:
  `Registered camera ... (imx378)`; *not* the IMX582 the app also supports). Mic exposed via UAC.
  Camera has no calibration data ("EEPROM data unprogrammed").
- Opal.app v0.26 (build 25.992). Copies in this directory: `Opal.app`, `Opal Backup.app`
  (identical); installed copy in `/Applications/Opal.app`.
- Built on a **private Luxonis depthai-core 2.15.1 fork** (`Opal.app/Contents/Frameworks/libdepthai-core.dylib`,
  commit 4386d6e1, closest public = branch `audioproc`), with a **private device firmware**
  `f57b4691…` (closed Myriad binary — can't be patched; only the *pipeline* is ours to change).
- USB PIDs: `f63c` = bootloader ("Luxonis Bootloader"), `2485` = Myriad ROM USB boot,
  `f63b` = firmware booted by the Mac (Opal app), `f63d` = firmware booted from flash (standalone webcam).
- Components: `Opal` (menu-bar app) → `OpalCameraDeviceService.xpc` (talks depthai to the camera,
  uploads firmware+pipeline, video to Mac) → `OpalCameraVideoService.xpc` → CMIO system extension
  `com.opalcamera.OpalCamera.opalCameraExtension` (virtual camera "Opal C1") via root shim
  `com.opalcamera.cameraExtensionShim`. LaunchAgent `com.opalcamera.OpalCamera.startOnUsbPlugged.agent`
  launches the app when a **`f63b`** device attaches (it does NOT match `f63d`).
- Camera is plugged into a **CalDigit TS5 Plus** Thunderbolt dock (USB port location `00220000`/`00120000`).
  Host: Mac16,9, sleep disabled.

## What has been changed (2026-10-31, with the user's go-ahead)

1. **Bootloader 0.0.4 → 0.0.15** (`bb9e4c86…`, the one bundled in Opal.app). Verified OK.
   Effect: host boots no longer go through the ROM (`2485`) detour; ~7 s app takeover.
2. **Flashed standalone webcam program** = Opal's own standalone pipeline (4K sensor, ISP 1/2 →
   1080p UVC, 180° rotation, AF auto, 60 Hz anti-banding, mic +28 dB) **plus an idle-watcher Script**
   that stops the sensor 3 s after the last UVC client leaves and restarts it when one connects
   (reads GPIO 58, which the UVC node drives on stream on/off). Enumerates as `f63d`.
   Restore Opal's original with `tools/bin/opaltool flash orig`.

## Key findings so far

- Thermal (host-booted, 4K30, `tools/thermal_run2.log`): streaming chip temp 46.8→49.4 °C after 5 min
  and rising; sensor stopped 42.4→37.3 °C and falling; CPU 27 % → 2 %.
- Opal app only *logs* (Sentry breadcrumb) avg chip temp > 65 °C ("Painful temperature!") and fps < 30.
  No throttling. Firmware has a thermal-shutdown path: "Temperature above {}C. Device will shutdown".
- App-mode streaming decision (Opal process log): `StreamingState: appsConnectedToPlugin / isPopoverOpen /
  CamEx → streamingOn|streamingOff`. Sensor runs whenever a meeting app uses "Opal C1" **or the
  popover is open**; otherwise `setStopStreaming` is sent (verified 2026-10-31).
- The app hard-resets the camera on every app start, `-[OpalCameraInput teardown]`, and when it finds
  an `f63b` device during `-[DeviceManager search]` (vendor request `bmRequestType=0 bRequest=0xF5
  wValue=0x0DA1` = "UVC device reset"). `search` polls every 100 ms for up to 30 min — that's the
  blinking icon.
- **Intermittent firmware crash**: `Fatal error on MSS CPU: trap: 00, address: 00000000` ~2 s after the
  sensor starts streaming, seen 2 of ~15 host boots (both on the first boot after switching firmware).
  Camera recovered via watchdog in those cases. **Leading suspect for the hang**, not yet observed in
  the wild. A stuck camera that is absent from USB entirely (as on 2026-10-30 → 31, 23 h, no
  enumeration even when the dock reconnected) fits a crash/thermal shutdown with no recovery.
- Standalone (flash-booted) firmware does not answer XLink attach; you can't read temps from it
  without rebooting it into bootloader (which `opaltool hostboot` does).

## App internals (reverse-engineered 2026-10-31)

- **Frame path**: camera (1080p NV12 per `OCResolution`; 2160p/1440p possible, 720p on USB2) → XLink →
  `OpalCameraDeviceService` → IOSurface → `OpalCameraVideoService` (MetalPetal on the Mac GPU) → CMIO
  extension, which only offers **1920×1080 `420v`** to meeting apps.
- **Filters**: 8 of 13 looks are stock Core Image `CIPhotoEffect*` (see `*_pipeline.json`); D1/G1/Q1/S1/X1,
  bokeh compositing, touch-up, skin tone, zoom/framing are MetalPetal (`MTI*`) on the Mac. On the camera:
  face detection NN (always while streaming), Google-Meet segmentation NN (bokeh), hand-pose NNs
  (gestures), PDAF focus, SystemLogger. Mac-side cost while streaming ≈ 30 % of one core total and barely
  changes with effects; camera-side cost does (MSS CPU 13 % → 29 %, +3 °C).
- **Network**: Sentry (crash reports + breadcrumbs), Mixpanel analytics (token 3943…, events incl. camera
  serial; macOS flags it "known tracker"), update checks on `opalcamera.sfo3.digitaloceanspaces.com`.
  No code uploads frames/images anywhere.
- **Injectable**: main app and `OpalCameraDeviceService` have `allow-dyld-environment-variables` +
  `disable-library-validation` (not sandboxed). XPC services don't inherit env, so only the main app is
  reachable via `DYLD_INSERT_LIBRARIES` (`open -a Opal --env DYLD_INSERT_LIBRARIES=… --args devMode`).
  `devMode` CLI arg enables a hidden Developer Mode (UI entry point not found yet).
- **Camera telemetry in app mode**: `tools/opalspy/opalspy.dylib` hooks
  `-[Opal.DeviceXPCConnectionHelperService newLogDataRecived:]` (arg = pointer to 11 floats: DDR used/total,
  CMX used/total, CSS heap used/total, MSS heap used/total, CSS cpu %, MSS cpu %, chip °C) and writes
  `~/Library/Logs/opal-watch/telemetry.log` (only while streaming). It also dumps the app's settings to
  `~/Library/Logs/opal-watch/opal-settings.txt`. `tools/opalspy/cpu-sampler.sh` logs Mac-side CPU.
- **App-mode measurements**: no effects 46 → 53 °C in 3 min; + focus lock/bokeh/skin/I2 → 56.3 °C at 6.5 min,
  still rising. CMX 2.39/2.50 MB (96 %) used. Stand-alone bare 4K30 for comparison: 49 °C at 5 min.
- **Streaming flapping**: popover open/close toggled `streamingOn/Off` 13× in 34 s (two flips 18 ms apart)
  → each sends sensor stop/start. Given the MSS crash ~2 s after stream start, this is a prime hang suspect
  and the best candidate for a debounce fix.
- Settings of note: `OCIsStartAtLoginOn = 0`, `OCIsStartWhenCameraIsPluggedInOn = 1` — Opal relies on the
  USB agent, which only matches PID `f63b`; with the new `f63d` standalone firmware it won't auto-launch.
  Fix: `tools/opal-launch/` (see below). Opal also opens its popover on every launch, which keeps the camera
  streaming until the popover is closed.

## Streaming debounce (in `tools/opalspy/debounce.m`, built into opalspy.dylib)

- Opal → device service call: `-[OpalCameraDeviceServiceProtocol changeStreamingState:(NSInteger) withState:(struct
  {iiiiiiBBBBBdC}, 48 B)]`; state 2 = on, 1 = off. It goes out via the NSXPC proxy class
  `__NSXPCInterfaceProxy_OpalCameraDeviceServiceProtocol` through CoreFoundation's fast path
  **`-[_NSXPCDistantObject _forwardStackInvocation:]`** (not `forwardInvocation:` — that cost several attempts).
- Policy: "off" is held 5 s; any newer call cancels it; when due it is forwarded only if CoreMediaIO says the
  "Opal C1" virtual camera is not running anywhere (`kCMIODevicePropertyDeviceIsRunningSomewhere`, see
  `cmio_inuse.m`; lookup failure → forward, i.e. original behavior + 5 s). "On" and everything else pass
  through immediately. Log: `~/Library/Logs/opal-watch/debounce.log`. Disable with env `OPALSPY_NO_DEBOUNCE=1`.
- Verified: normal close → off forwarded after 5 s, driver logs `Setting streaming on : 0` 5 s later.
  Not yet verified in the field: the cancel path (needs a real popover flap) and the in-use check's value
  inside Opal (debounce.log prints it 3 s after each "on").
- Found an Opal/CoreMediaIO edge case: if an app reopens the virtual camera ~1 s after the previous client
  closed, the extension never gets `startStream`, CMIO reports the device as not running, and the client gets
  stale frames while Opal turns the sensor off. Not fixed (no reliable signal).

## Patching the app bundle: dead end (tried 2026-10-31)

`tools/opalcore/` builds `opalcore.dylib` for the camera-driver service (debounce at
`-[OpalCameraInput changeStreamingState:withState:]`, depthai log tap via `DeviceManager._device`, 30 s chip
temperature RPC) and `make-patched-app.sh` adds an `LC_LOAD_DYLIB` to that service in a fresh copy of the app.
It does not run:
- Changing nested code breaks the outer seal of the team-signed app → **AppleSystemPolicy kills the main app**
  ~50 s after launch ("Security policy would not allow process"). An unmodified fresh copy runs fine.
- Re-signing the whole app ad-hoc would run, but the root shim requires XPC clients signed by team
  `97Z3HJWCRT` (`anchor apple generic and certificate leaf[subject.OU] = "97Z3HJWCRT"`) → no virtual
  camera, unless the root helper's auth is weakened (don't).
- Also: macOS App Management protection blocks writing into a launched/known app bundle, even outside
  /Applications (hence `make-patched-app.sh` copies with `ditto --noextattr` first).
Working route: unmodified app + `opalspy.dylib` injected into the main app at launch (`tools/opal-launch`).
Device-service-level features (camera log tap, idle telemetry) are therefore not available yet.

## Auto-launch with recorder (`tools/opal-launch/`)

`opal-launch` (launchd job `local.opal-launch`) starts Opal with `DYLD_INSERT_LIBRARIES=tools/opalspy/opalspy.dylib`
at login and on Opal C1 attach (PIDs f63b and f63d), and replaces Opal's own USB agent (disabled per-user).
**Must be installed by the user** (`tools/opal-launch/install.sh`; `install.sh uninstall` reverts) — the
agent's permission policy blocks installing persistent launch items that inject code. Log:
`~/Library/Logs/opal-watch/opal-launch.log`.

## Tooling (all in `tools/`, see `tools/README.md`)

- `tools/opal-watch/` — **background recorder, installed and running** as LaunchAgent
  `local.opal-watch` (`~/Library/LaunchAgents/local.opal-watch.plist`). Writes
  `~/Library/Logs/opal-watch/YYYY-MM-DD.log`: USB attach/detach of `03e7` devices + dock, all
  `com.opalcamera.*` debug logs (app streaming decisions, device service lifecycle, CamEx start/stop),
  USB/process snapshots on change (+ every 30 min), new Opal crash reports. 30-day retention.
  - `tools/opal-watch/opal-watch-report [hours] [--all]` — condensed timeline.
  - `tools/opal-watch/opal-capture-state [note]` — freeze state into
    `~/Library/Logs/opal-watch/incident-<ts>/` (read-only; run BEFORE replugging).
  - Restart: `launchctl kickstart -k gui/$(id -u)/local.opal-watch`.
- `tools/bin/probe` — passive depthai device list + state (safe while the app runs).
- `tools/bin/opaltool` — `hostboot orig|idle <secs>` (RAM-only test w/ temps + camera logs),
  `flash orig|idle`, `blversion`, `blflash` (don't). `tools/bin/thermal A B C` — temp experiment.
  Build: `tools/fetch-deps.sh` then `tools/build.sh <src.cpp> <out>`.
- **Before running opaltool/thermal, stop the Opal app and its USB agent** (it grabs the camera on
  every re-enumeration):
  `launchctl bootout gui/$(id -u)/com.opalcamera.OpalCamera.startOnUsbPlugged.agent; osascript -e 'tell application "Opal" to quit'`
  and restore afterwards:
  `launchctl bootstrap gui/$(id -u) /Library/LaunchAgents/com.opalcamera.OpalCamera.startOnUsbPlugged.agent.plist; open -g -a Opal`

## Hang playbook

1. **If the camera is still stuck (not yet replugged):** run
   `tools/opal-watch/opal-capture-state "<what the user saw>"` first. Then look *before* replugging:
   - `tools/bin/probe` — is the Myriad visible to depthai at all, and in which state?
   - `ioreg -p IOUSB -w0 | grep -iE "opal|luxonis|movidius|VSC"` — is it on the USB bus at all?
   - Is it hot (ask the user)? Is the Opal app running (`pgrep -fl Opal.app`)?
   Only then have the user replug, and capture again after it comes back.
2. **Read the timeline**: `tools/opal-watch/opal-watch-report 48` (and the incident folder).
   Classify the failure:
   - *Camera vanished from USB and never came back* (last `terminateDevice` with no later
     `enumerated`): crash/thermal shutdown with no recovery. Note what preceded it: how long it had
     been streaming (`StreamingState ... streamingOn` without a matching `streamingOff`), popover
     left open, dock (`TS5 Plus`) events, screen lock/teardown, any `Resetting device` / `UVC device
     reset` just before.
   - *Camera present on USB but app can't connect* (USB snapshot shows `f63b`/`f63d`/`f63c` while the
     app logs `search` / `No device available` / `cameraNotFound`): firmware wedged with USB alive,
     or the app's reset/boot sequence failing. Check which PID and whether `UVC device reset` loops.
   - *Stuck in bootloader* (`f63c` persists): flashed program failed to boot; consider
     `opaltool flash idle` (ask first).
3. Correlate with `Low FPS`/`Painful temperature`/heartbeat messages and with `pmset -g log`
   (sleep/wake/display) and kernel Thunderbolt events in the incident folder.
4. Report findings to the user in plain language, update this file's "Key findings", and propose
   the next experiment (e.g. stress-test MSS crash frequency with repeated `opaltool hostboot`,
   lower thermal load in app mode, a host-side auto-recovery watchdog).

## Gotchas

- In this zsh, `log` is a shell builtin: always call `/usr/bin/log`. zsh also doesn't word-split
  `$VAR` and treats `$c:cmake` as a modifier — use bash scripts for loops/flags.
- The user has the **Hopper demo** (REA provider): it times out quickly. Keep sessions short;
  prefer `otool -tV`, `strings`, `nm` for simple lookups.
- No Rosetta; depthai 2.15 has no arm64 PyPI wheel — that's why tools link Opal's own dylib.
  Header ABI needs `-DXLINK_USE_MX_ID_NAME` (verified `sizeof(dai::Device)==0x2b8`). Node sizes match
  for ColorCamera/UVC/UAC/Script/XLinkIn/Out/AudioMic; **NeuralNetwork nodes do not** — don't create them.
- Host-booting a UVC pipeline requires `BoardConfig.uvcEnable = true`; on-camera `GPIO.read(58)`
  requires a non-exclusive `GPIO.setup(58, GPIO.OUT, GPIO.PULL_NONE, False)` first.
- Use the IMX378 tuning blob `Opal.app/Contents/Resources/Opal_v0.12.bin` (`Opal582_v2.bin` is for IMX582).
