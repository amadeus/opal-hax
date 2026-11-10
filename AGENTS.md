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

- **OWNER'S OBSERVATION (2026-11-01, authoritative): the hangs happen IN THE MIDDLE of a video session — the
  camera dies while streaming. Never at stream start or stop.** So the start/stop theories below (MSS crash
  ~2 s after stream start, popover flapping, resets) are NOT the explanation for the real hangs. Look at what
  accumulates during a long stream: heat (chip temp in camera.log `[telemetry]` every 30 s and telemetry.log
  every 5 s), memory (CMX 96 % used), NN load, USB/dock errors. The custom build now records all of these.
- **Overnight soak 2026-11-01 (custom build, 4K output, Discord call, lens covered, bokeh+skin+I2+focus lock on):
  4 h 22 min continuous stream (00:26 → 04:48), NO crash.** Ended when Discord closed the camera itself (client
  `stopStream`), camera healthy to the last second. Chip: 47 °C → 58 °C at 10 min → plateau 58.5–61 °C (peak 61.0)
  for 4 h. CSS 56–64 %, MSS 63–72 %, DDR 143/337 MB flat (81 MB at 1080p), CMX 2.39/2.50 MB flat, sensor 30.1 fps
  throughout. So a long 4K stream by itself does not hang it in a cool room; heat plateaus below the app's 65 °C mark.
  Record: `~/Library/Logs/opal-watch/overnight-2026-11-01.log`, camera.log, telemetry.log.
- **Daytime Google Meet call 2026-11-02 07:55 → 09:15 (80 min, 1080p, bokeh+skin+focus lock, display on): hottest yet,
  NO crash.** 37 °C → 58 @10 min → 64.7 @25 min → 66 @45 min → 69.3 @80 min, still creeping ~0.5 °C/10 min (single
  samples up to 69.4). Same camera load as the overnight soak (CSS ~54 %, MSS ~70 %), sensor 58 °C, 30.1 fps steady,
  no errors. Ended cleanly (client stop → debounce → off; 54.6 °C 30 s later). ~5–9 °C hotter than the night run at the
  same load → environment (room/display heat/sun), not the client app (the camera's work doesn't depend on it).

- **PROFILED 2026-11-05 (`tools/profile/`, see tools/README.md "Camera profiler"): the "high load" state of every long
  call (CSS 55 % / MSS 70 %, 60–69 °C in the Meet call and the overnight soak) is the HAND-POSE pipeline running with every
  gesture OFF.** Opal sends `gesturesCtrlQueueChokeEmbedded [1]` ("CAMERA:: Setting choke on: 1") at every stream start;
  un-choked, the on-camera manager Script re-enables the palm-detection ImageManip by itself and the three hand-pose NNs
  run at full rate (the host's `skipCurrentImage`/`processInputs=false` don't stop them), and face/segmentation drop from
  ~25 to ~15 inferences/s. Measured on the camera (1080p, Opal's exact pipeline): plain stream CSS 25 % / MSS 6 %; + face +
  segmentation 30 / 28 (Opal's intended state with focus lock + bokeh, the "low load" sessions at ~57 °C); + un-choked
  manager 48 / 65; gestures fully on 55 / 73. The app telemetry history shows both states (telemetry.log: css/mss 29/28
  for hours on 10-31 and 11-04 vs 55/70 on 11-01, 11-02; the lo sessions had a choke 0 land after streaming started).
  Fix (2026-11-05, in opalcore.mm "gesture choke guard"): `-[ChokeQueueManager setChokeToActive:YES]` on the gestures
  choke singleton is turned into NO unless `setGesturesNNActive:YES` was seen (hooked on OpalCameraInput and the XPC
  facade), AND the manager is choked by opalcore itself 0.5 s after every camera connect: a pipeline restart (every
  resolution change) brings the Script back un-choked by default and Opal does not resend the gesture state (its cache
  still says "on", so it doesn't even call setChokeToActive: — seen 2026-11-05 23:07 after a switch to 4K: 55 % / 72 %
  again). Log lines `[choke] …` in camera.log; disable with env `OPALCORE_NO_CHOKE_GUARD`. Built, notarized and packaged
  as `Opal-0.26-custom.<ts>.pkg.zip` (repo root); the 2026-11-05 22:54 package (stream-start guard only) was installed by
  the owner 22:57 (then needed the extension restart described under installer notes); 2026-11-06 10:4x adds the
  connect-time choke, the 30 fps fix, the explicit stop and the installer shim fix.
- **4K output is USB-bound**: the camera links at 5 Gb/s (Myriad X USB 3.0; the dock hub is 10 Gb/s, irrelevant) and sends
  raw NV12: 12.5 MB/frame → ~24 fps delivered to the Mac (measured; 1080p: 26–30), ~40 ms per frame in transfer = the
  "laggy preview" at 4K. Camera load at 4K with face + seg: CSS 41 % / MSS 40 % (1080p: 30 / 28). Meeting apps get
  1080p from the virtual camera anyway; 4K only helps Opal's digital zoom. Recommend 1080p.
- **1080p was delivered at 26.4 fps, not 30 — FOUND AND FIXED (2026-11-06, `tools/opalcore/fpsfix.mm`)**: the sensor runs
  30.1 (4K readout; enum THE_4_K=2 in this depthai revision: THE_1080_P=0, THE_1200_P=1, THE_4_K=2, THE_5_MP=3,
  THE_12_MP=4) but the camera drops every 8th frame (sequence gaps, 66 ms holes) whenever a camera control message
  applied TWO OR MORE 3A commands at once — Opal's stream start carries 8 (START_STREAM + MOVE_LENS + AE_AUTO + AWB +
  SCENE + EXP_COMP + CONTROL_MODE + SATURATION) and its pipeline's initial control 8 more; the drop then lasts for the
  session. One command per message, 40 ms apart, keeps 30.0 fps with no gaps (back-to-back singles don't). Bisected on
  the UVC path (the flashed program, single commands, does 30.2 distinct frames/s) and confirmed over XLink
  (`profile … init.cmdMask=0 bare=1 seq=ae,awb,scene,exp,ctrl,sat` → 30.4, 0 gaps). Not the cause: ISP throughput,
  USB, XLink streams/queues/chunking, rotation, preview, video size, tuning blob, AE in the dark, AF mode. Fix in
  opalcore: every control message on Opal's "control" queue with 2+ commands is sent as single commands 40 ms apart,
  synchronously on Opal's thread (both DataInputQueue::send overloads hooked via the service's GOT; only the
  `const ADatatype&` one is used), and the ColorCamera initial control's mask is cleared at startPipeline and replayed
  the same way before Opal's first control message — through Opal's OWN RawCameraControl object (a copy made with our
  headers serialized wrongly → "Raw C++ exception" in the service → teardown loop; the service also throws when our
  code creates the "control" queue before Opal does). Verified through the virtual camera: 29.9 distinct fps over 60 s
  (was 26.4), CSS 37 % / MSS 36 % (was 32 / 30: more frames). Flag files in ~/Library/Logs/opal-watch for experiments:
  `fpsfix-noreplay`, `fpsfix-replay-subset`; env `OPALCORE_NO_FPS_FIX` disables. 1440p: 33 / 31 %, 62 ms (fine).
  4K H.264/H.265 on the camera encoder: 26.4 fps, 90–96 ms, 6 MB/s, but CSS 59 % / MSS 72 % — rejected. Raw 4K: 23.9 fps,
  229 ms (USB 3.0 5 Gb/s link).
- **Idle camera kept streaming after a (re)connect — FIXED (2026-11-06, opalcore "explicit stop")**: Opal applies "off"
  only when it believes the sensor is on; after a connect it believes "off" while a freshly booted camera streams by
  default (the pipeline's initial control has no stop), so with nobody using it the sensor ran for hours at ~58 °C
  (2026-11-04 10:50→18:39 after the popover closed... and 2026-11-06 03:22→10:32 after the night's last restore; the
  "lo" 29/28 % sessions in telemetry.log are this). Now whenever the debounce applies an off, opalcore also sends
  STOP_STREAM itself through Opal's control queue (`[debounce] explicit STOP_STREAM sent`); verified: fresh connect →
  idle 6 % / 0 % within 30 s. Opal's own quit does not stop the sensor either (teardown resets the camera instead).
- **Host-boot gotcha (2026-11-06)**: a camera host-booted in UVC mode (BoardConfig.uvcEnable) by our public-lib tools
  is left in its bootloader, and the next UVC-mode boot from that state (or any repeat in a series) crashes the firmware
  1.4 s after start (the old MSS trap); booting from the flashed program (f63d, what a vendor UVC reset of Opal's f63b
  firmware leaves) works. tools/profile runs that need UVC mode relaunch+quit Opal between runs for that reason.
- **Crash bisect clue (the old MSS trap)**: the app template with only ColorCamera + XLinkOut rgb (+ audio) crashes the
  firmware 1.4 s after start (`Fatal error on MSS CPU: trap: 00`), while removing any ONE of the groups {config XLinkIn,
  audio, LED Script+XLinkIn, SystemLogger+XLinkOut, control XLinkIn, all NN branches} does not. One of 1,2,7,8,31,32
  keeps it alive; not bisected further.
- Profiling limits: the firmware's RPCs are only CPU %, 4 chip temperatures (CSS/MSS/UPA/DSS, within 1 °C of each other),
  DDR/CMX/heap and log level (RTEMS' per-thread CPU report is compiled in but unreachable), so component costs come from
  ablation (`profile … face=0 seg=0 choke=1 gate=face:3 sensor=1080p rm=…`). The IMX378 1080p (2×2 binned) sensor mode
  works with Opal's firmware (same FOV as 4K + ISP ½, 30 fps to the host instead of 26, DDR 59 vs 81 MB, slower warm-up)
  but the picture is visibly softer at night (ISO 2400, sample frames in ~/Library/Logs/opal-watch/profile/frame-*.png),
  so it is not used.
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
- **Network** (all blocked in the custom build since 2026-11-01: DSN host + api.mixpanel.com → `.invalid`;
  build asserts none remain): Sentry (crash reports + breadcrumbs), Mixpanel analytics (token 3943…, events incl. camera
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

## Custom build: Opal re-signed under the owner's team (the chosen route, `tools/custom-build/`)

The owner has a paid Apple Developer team **UHD99KF9X7** (identity "Apple Development: Amadeus Demarzi
(J972UZ26TC)", Xcode signed in). `build_custom_opal.py` turns the pristine `Opal Backup.app` into
`build/Opal.app`:
- same-length swaps everywhere: `com.opalcamera`→`com.amadeus.oc`, `97Z3HJWCRT`→`UHD99KF9X7`,
  `group.97Z3HJWCRT.com.opalcamera.Opal`→`UHD99KF9X7.com.amadeus.oc.OpalShared` (team-prefixed: no profile),
  update host → `xxx….invalid`; renamed files; old ids fully gone (asserted).
- built in: `opalcore.dylib` (camera-driver service: debounce at source, camera log tap, 30 s chip temp)
  and `opalspy.dylib` (main app: telemetry, (no automatic popover any more: see the popover `CODE_PATCHES` below),
  settings import from `~/Library/Logs/opal-watch/opal-settings-export.plist`, exported by opalspy
  running in the original app — 43 OC* keys exported 2026-10-31).
- **No updating (2026-11-01, owner's request: "this is an app I control now")**: main executable thinned to arm64;
  `CODE_PATCHES` make UpdateManager (schedule/check/download/launch-install), the FirmwareUpdateManager "New
  Firmware Update" prompt + flash closure, `-[FirmwareManager flashBootloader…/flashApplicationPipeline…/
  updateCalibrationVersion]` and dev-mode `forceFlashCamera:` return on entry (original bytes asserted), so the app
  can never re-flash the camera (only the main app links depthai's flash APIs; the device service's "Flashing
  device" log is just the RAM boot). `OpalCameraUpdateXPC.xpc` (pkg installer) deleted. The Updates settings panel
  (version, git hash, auto-install toggle, changelog) is hidden at runtime by `tools/opalspy/hidepanels.m`: it points
  UpdatesPanelView's SwiftUI `View` witness table at EmptyView (Body type, Body:View conformance, no-op body getter);
  log line "hidepanels: UpdatesPanelView hidden" in opalspy.log. `DATA_PATCHES` shrinks the Settings page
  height constant 547 → 444 pt (the 4 remaining panels; gap above Quit then equals the 10 pt between groups). The popover chrome height with a tab open (preview + tabs 344 pt + 45 pt Quit footer) is patched
  389 → 385.5 pt: the gap below Quit (was 13 pt) is 9.5 pt, 1 px less than above it, which looks even next to the
  popover's inner border.
  **Audio > Noise cancellation** panel hidden the same way (owner's request; NoiseCancellationPanelView, not a
  persisted setting) and the Audio page's fixed row count patched 3 → 2 (`CODE_PATCHES`, TabManager page height =
  rows × 72 + (rows−1) × 10 pt). TabManager pages: 1 Camera, 2 Audio, 3 Effects, 4 Manual, 5 Settings.
  **Feedback removed** (owner, 2026-11-09, abandoned app): every tab's "Send Feedback" button (`SendFeedbackButton`, a
  field-less struct) now acts as Opal's own `QuitButton` (hidepanels copies QuitButton's Body type, Body:View conformance
  and body getter into SendFeedbackButton's View witness table; log "SendFeedbackButton now acts as QuitButton"); the
  feedback form `SendFeedbackView` (Sentry "User Feedback" submit), `FeedbackModeHeaderView` and `FeedbackModeFooterView`
  ("email hello@opalcamera.com") are EmptyView; and a `CODE_PATCHES` entry makes the `Storage.isFeedbackViewVisible`
  setter always store false, so feedback mode can't open by any path. hidepanels now finds types by name in
  `__swift5_types`/`__swift5_proto` (no hardcoded addresses).
  **Looks thumbnails play live** (owner, 2026-11-09: they updated once a second): the video service renders each look's
  thumbnail at a per-look fps the app sets (`adjustFPSOfFilterWithID:andFPS:`): 30 while hovered, 1 otherwise. A
  `DATA_PATCHES` entry makes the "pointer left" value 30 and opalspy `filterfps.m` asks for 30 for every look as its
  thumbnails arrive (hooks `-[VideoXPCConnectionHelperService receiveWithSurface:forName:]`, finds the connection by
  `setRemoteObjectInterface:`); logs measured rates ("filterfps:") every 10 s. Disable: `OPALSPY_NO_FILTER_FPS=1`.
  **The popover only opens when the menu-bar icon is clicked** (owner, 2026-11-04): `CODE_PATCHES` NOP Opal's own
  calls that open it on camera connect/frames (`showPopover()` from DeviceXPCConnectionHelperService, 5 sites). The
  launch call (`[appDelegate togglePopover:nil]`) can't simply go: it is also Opal's setup (loads the camera view
  controller; without it the icon stays faded and the camera doesn't connect until the first click, seen
  2026-11-04), so opalspy `launchsetup.m` runs a sender-less togglePopover: with NSPopover show and NSApp activate
  skipped. The click action (sender = status button), recording re-shows and URL opens (no scheme) are kept.
  Clicking the icon with the popover open used to close and immediately reopen it: on this macOS the click first
  deactivates Opal, `applicationWillResignActive:` closes the popover, then the click's `togglePopover:` finds it closed
  and shows it. Opal sets `popover.animates = false` (instant close); a `CODE_PATCHES` entry turns animation back on, so
  the popover is still closing when the action arrives and the toggle closes it (reproduced with a stand-in app).
  Replaces opalspy's old launchquiet hook (time-window blocking).
  **Status readout** (owner, 2026-11-06; opalspy `statushud.m`): while the popover is open, a small label over the
  bottom-left of the live preview shows chip °C, camera CPU CSS/MSS %, received fps + frame height, and the Mac CPU of
  Opal's processes (% of one core); tooltip adds camera DDR/CMX, latency, meeting-app use. Sources: the telemetry feed
  (`newLogDataRecived:`, already hooked for telemetry.log) and the per-frame callback `-[DeviceXPCConnectionHelperService
  imageStreamResponseWithYUVSurface:andDepthSurface:andDetection:andDeviceInfo:andVideoLatency:andAudioLatency:]` (IOSurface
  + 2 structs passed by pointer). The preview is found as the largest view drawing a CAMetalLayer; the popover's view
  tree is logged once to opalspy.log. Orange ≥ 65 °C, red ≥ 75 °C. Disable: env `OPALSPY_NO_STATUS_HUD=1`.
  Kept: camera-extension "update" logic (CamEx requiresUpdate) — that re-activates our own extension, it's not
  app updating. `update-custom-opal.sh` now keeps the replaced build in `previous-custom/<ts>/`.
- **Release builds = Developer ID + notarized (2026-11-01)**, so they run on any Mac and outlive the certificate:
  `build_custom_opal.py --developer-id --profile build/Setup-DeveloperID.provisionprofile` (identity "Developer ID
  Application: Amadeus Demarzi (UHD99KF9X7)", secure timestamp; the Developer ID profile "Opal Setup Developer ID"
  provisions all devices, expires 2044-10-27), then `notarize.sh` (notarytool keychain profile `opal-notary`,
  staples, spctl check). The development identity/profile path still works without `--developer-id` (dev cert
  expires 2027-05-09, dev profile only covers this Mac). `EXT_BUNDLE_VERSION` (now 11) sets the camera extension's
  CFBundleVersion: macOS only replaces an installed system extension when that changes — bump it whenever the
  extension must be re-signed. **Custom-to-custom upgrades must NOT restart the root shim daemon** (fixed in
  `pkg/preinstall` 2026-11-05): the running camera extension is registered with the old shim process, a new one has no
  endpoint for it, and Opal shows "Restart to finish update" (Setup log "Was unable to retrieve endpoint from shim …
  requires a reboot") until a reboot or the extension process is restarted. Seen on the 11-01 and 11-05 upgrades.
  Opal's own Setup app declines replacement (OSSystemExtensionError 11) unless run with
  env **`ForceReinstall=1`** (`open -n --env ForceReinstall=1 ".../Opal Camera Setup.app"`). After a replacement
  the new extension does NOT start until a reboot (launchd "Submit job failed … Operation already in progress"
  while the old one terminates); Opal shows "Restart to finish update" and the camera is unavailable until then.
- **Installer package** for other Macs: `make-pkg.sh` → `build/Opal-<ver>-custom.<ts>.pkg` (Developer ID Installer
  signed, notarized, stapled). `pkg/preinstall`: stops Opal; removes the original's launchd items, helper tools,
  legacy DAL plugin `/Library/CoreMediaIO/Plug-Ins/DAL/OpalVirtualCamera.plugin`, receipt; exports the original's
  OC* settings (`defaults export group.97Z3HJWCRT.com.opalcamera.Opal`, filtered by `pkg/export-oc-settings.js`)
  for opalspy's first-launch import; removes the original extension via its ForceUninstall mode; moves the original
  app to the user's Trash; deletes a previous custom app. `pkg/postinstall`: shim daemon + USB agent (f63b+f63d),
  extension activate/replace via ForceReinstall when the bundled version isn't already active (opens Camera
  Extensions settings if it's waiting for the user), launches Opal. No settings are shipped (owner's choice: a Mac
  keeps its own Opal settings, else defaults). Log: `/var/log/opal-custom-install.log`. Tested on this Mac
  (upgrade path) and on the owner's laptop (fresh path) 2026-11-01. Laptop lesson: it also had an older Opal "v2"
  (`/Applications/Opal.localized/Opal.app`, extension `com.opalcamera.Opal.v2.opalCameraExtension`, settings domain
  `97Z3HJWCRT.com.opalcamera.v2.Opal`), and Installer "upgraded" THAT bundle in place instead of installing to
  /Applications/Opal.app (pkg is non-relocatable, still happened). So preinstall now handles every `Opal*.app` under
  /Applications and ~/Applications (originals: settings export, own ForceUninstall per extension id, Trash, empty
  folder removed; stray custom copies deleted), and postinstall moves the app to /Applications/Opal.app if it landed
  elsewhere. The v2 extension on the laptop stays registered (its app was overwritten, so nothing can uninstall it
  without SIP off); harmless, it publishes no camera.
- **Extension approval guide** (`tools/opalspy/extguide.m`): macOS requires the user to approve the camera extension
  (Login Items & Extensions > Extensions > Camera Extensions (i) > Opal C1) and its own "Camera Extension Blocked"
  alert points at Privacy & Security, which confused the owner. When Opal sees our extension `waiting for user`
  (polls `systemextensionsctl list` for 2 min after launch) it shows step-by-step instructions + "Open Settings",
  then confirms "Opal C1 is ready" once enabled. Untested (both Macs already approved). Disable: `OPALSPY_NO_EXT_GUIDE=1`.
  The pkg no longer opens System Settings itself.
- **Latest installer is committed at the repo root** as `Opal-<version>.pkg.zip` (built by make-pkg.sh).
- **Camera firmware updater (2026-11-01, built but deliberately NOT tested — owner's call):** `tools/opalspy/
  firmwareoffer.m` watches (IOKit) for an `IOUSBHostInterface` 03e7:**f63b with bInterfaceClass 14** = a camera
  running Opal's ORIGINAL flashed program (Opal-booted cameras are f63b without a video interface — verified on the
  owner's camera; ours is f63d). It shows a critical alert with an explicit brick-risk / "at your own risk" warning
  ("Not Now" is the default button; asked again next launch). On accept it opens `Opal.app/Contents/Helpers/Opal
  Camera Updater.app` (`tools/camera-updater/`, links Opal's libdepthai-core/libusb) and quits Opal. The updater
  boots out the USB agent, resets the camera into its bootloader, flashes the bootloader only if
  `getFlashedVersion() < getEmbeddedBootloaderVersion()` (same calls as `opaltool blflash`), flashes the idle-watcher
  standalone program (`standalone.hpp`, anti-banding 50/60 Hz from the Mac's region, auto for JP), waits for f63d,
  restores the agent, restarts Opal. Log: `~/Library/Logs/opal-watch/camera-update.log`. Disable the offer with env
  `OPALSPY_NO_FIRMWARE_OFFER=1`. The owner's camera never matches (it's f63d).
- entitlements = originals minus team-restricted keys; only the Setup app ("Opal Camera Setup.app",
  `com.amadeus.oc.OpalExtensionInstaller`, hosts the camera system extension) needs a provisioning
  profile for `system-extension.install`: `get-profile.sh` (CHANGES THE DEV ACCOUNT — ask first) uses a
  throwaway Xcode project + `xcodebuild -allowProvisioningUpdates` (with `DEVELOPER_DIR`, since
  xcode-select points at CLT) and writes `build/Setup.provisionprofile`; then rebuild with `--profile`.
- **INSTALLED 2026-11-01 00:03** (originals in `rollback/20261101-000311/`). Profile obtained via
  `get-profile.sh` (Xcode account had to be re-added first). After install the new extension sat in
  `[activated waiting for user]` until the owner enabled it under System Settings → General → Login Items &
  Extensions → Camera Extensions (ⓘ). Verified then: shim daemon running, new extension running and
  connected, opalcore loaded in the device service (`camera.log`: hooks installed, depthai log tap attached,
  camera logs flowing), debounce hold/apply AND cancel path ("pending off superseded"), launchquiet,
  USB agent fires on `f63d`. Meeting app verified 00:10 (Discord): served by the new extension, `CamEx: true` →
  streamingOn; Discord stopped and restarted its stream 2 s apart and the debounce kept the sensor on
  ("pending off superseded"). Chip 52.7 °C / CSS 52 % / MSS 70 % while streaming in a dark room (gain 24x).
  Old extension (team 97Z3HJWCRT) removed 00:13: `systemextensionsctl uninstall` needs SIP off, so we used
  Opal's own uninstall mode — the Setup app checks the ENVIRONMENT variable `ForceUninstall` (not an argv), and
  the deactivation must come from an app inside /Applications (else OSSystemExtensionError 3). Recipe:
  `ditto --noextattr --noqtn rollback/<ts>/Opal.app "/Applications/Opal Original (temp).app"`,
  `open -n --env ForceUninstall=1 ".../Opal Camera Setup.app"`, then delete the temp copy →
  `[terminated waiting to uninstall on reboot]`. The same env var on OUR Setup app would remove our extension.
- `install-custom-opal.sh` (owner runs it; sudo): moves /Applications/Opal.app + all com.opalcamera.*
  launchd plists to `rollback/<ts>/`, installs build, adds `com.amadeus.oc.cameraExtensionShim` daemon and
  a USB agent matching PIDs f63b+f63d. `rollback-custom-opal.sh` reverses it.
- The original Setup app can deactivate its extension (`deactivationRequestForExtension`, "Setup
  uninstall requested") — trigger not yet identified; otherwise macOS drops it once its app is gone.

## Windows port (started 2026-11-01, `tools/win/`, details in `tools/win/README.md`)

Owner's goal: all Opal Mac app features on a Windows PC. Approach: public depthai-core at `a3c01e446` (the commit
Opal's private fork is based on, found by matching bootloader `bb9e4c86` + depthai-shared `0b456575`) + Opal's
firmware extracted from Opal.app + Opal's exact app-mode pipeline captured from the running custom build
(`tools/opalcore/pipecap.mm`, off unless `~/Library/Logs/opal-watch/pipecap/ENABLE` exists; session 1 recorded with
the owner toggling every feature: `tools/vendor/opal-capture/session1`, gitignored). The template rebuild is
byte-identical to Opal's and streams video, face detection, segmentation, hand pose, audio and sysinfo from the
camera on macOS. The camera core (`tools/win/core`) is tested on the camera on macOS; the Windows app (effects,
overlays, gestures, tray UI), the virtual camera DLL and build/install scripts are written and compile-checked with
MinGW but NOT yet run on Windows. Windows 11 only (owner's call). The owner does NOT want to compile on the PC: build it here with
`tools/win/build-windows-on-mac.sh` (MinGW cross-compile, static, system DLLs only) -> `build/Opal-Windows-<date>.zip`;
the PC just runs Opal.exe, which installs itself (a .cmd/PowerShell installer got the download flagged as a virus
by Chrome/Defender; binaries are unsigned). Opal assets never go through git. The flashed f63d
program exposes a WinUSB interface (MS OS descriptor) so XLink can reset/boot it on Windows without drivers.
Looks: all 13 are per-pixel and baked into LUTs from Opal's own code (`tools/win/luts`); Opal converts camera NV12
with BT.709 full range.
Video effects (2026-11-01): every Windows effect is now a transcription of Opal's own (bokeh + person mask, touch-up,
skin tone, highlights/shadows, pixellate, Face Lock Kalman framing, zoom, spotlight, mirror, look blend, YUV
conversions), in Opal's order at the camera resolution with 8-bit intermediates, checked against Opal's Metal shaders by
`tools/win/verify/shader-check.sh`; specs and known gaps (CLAHE/contrast, vibrance not ported) in `tools/win/verify/`.
UI (2026-11-02, owner: "doesn't feel windows native"): rebuilt as Windows 11 Settings-style cards (`app/fluent.cpp`, WinUI
metrics/colours/accent/light-dark, smooth scrolling), screenshot on the Mac with `tools/win/preview/preview.sh`. Tray: with
NOTIFYICON_VERSION_4 a left click sends WM_LBUTTONUP and NIN_SELECT; toggling on both made the popover flash shut. Mic
mute added (Opal's `AudioInConfig.disableOutput`).
Observed 2026-11-02 on the PC: with Opal quit, the flashed f63d program restarted ~1 s after every boot (F63C 6 s -> F63D
1 s -> gone, looping). Disabling the f63d's WinUSB function (MI_00 "S_BULKDATA", pnputil /disable-device) stopped it;
the same program host-booted by `tools/win/webcamtest` (OpalWebcamTest.exe, RAM only) ran fine for 40 s. After a PC
restart (MI_00 re-enabled) the loop was gone, so it was a stuck Windows/USB state, cause unknown. If it recurs: a
USBPcap/Wireshark capture of the loop shows what Windows sends before the camera drops.
Testing it on the camera means taking it from Opal: boot out the USB agent and quit Opal first, restore after.
Observation: a hand-made `ColorCamera.video → XLinkOut` pipeline makes Opal's firmware crash with the same
`Fatal error on MSS CPU: trap: 00` signature as the old intermittent crash, ~1.5 s after streaming starts, every
time, with Opal's own library too (autofocus off did NOT avoid it; one clean run was a fluke). Opal's real pipeline
does not crash. Worth bisecting later: it may explain the original hangs.

## Opal Composer (branch `composer-hax`, static analysis only, 2026-11-01, NOT run yet)

`Opal Composer.app` (repo root, untracked) = Opal's successor app, **v2.0 build 24, signed 2024-10-18**, bundle id
`com.opalcamera.Opal.v2` (the "Opal v2" found on the owner's laptop). For the C1 and the Opal Tadpole (a Linux camera,
USB 0x3673:0002, own UVC/protobuf service + `tadpole_firmware.tar.gz` kernel/ubifs). Same architecture as Opal 0.26
(device service with depthai → video service → CMIO extension `com.opalcamera.Opal.v2.opalCameraExtension` + shim),
plus Sparkle updates (`updates.opal.camera`, still resolves → Squarespace), Sentry, Mixpanel. Its depthai is a newer
private build: lib commit `24f609f6`, device firmware `e2929eb3`, bootloader blob `7958b241`. Main app is hardened
runtime without dyld-env/library-validation exemptions (no injection without re-signing); the device service has them.

How Composer sees a C1 (`+[FirmwareManager isOpalDevice:idProduct:maxPower:]`, libusb scan):
model 1 = `03e7:f63b` with config `bMaxPower == 5` (a camera running the ORIGINAL flashed program; an app-booted f63b
has a much higher MaxPower, so Composer ignores cameras another app has booted), model 2 = `03e7:f63d` (newer flashed
program, what ours enumerates as), model 3 = Tadpole. Camera "firmware version" = USB bcdDevice (major.BCD minor);
Composer's bundled C1 firmware is **4.10** (`getEmbeddedC1FWVersion`), legacy = 2.0. Our f63d's bcdDevice is unknown
(app-booted f63b reports 0x0200).

**Why it never worked with this camera: the IMX378 gate.** `checkForConnectedCamera:` (onboarding camera check) loops
until a C1 is found; for model 1 it host-boots Composer's firmware (RAM only, `getSensorModel` →
`DeviceBase::getCameraSensorNames`) and if the sensor is `IMX378` it fails with error domain `com.opalcamera.imx378`:
"The connected camera is not supported by Opal Composer and requires the original Opal Camera application…". This
camera is IMX378 and was on the original flashed program (model 1) → blocked in onboarding. Composer still carries
IMX378 bits (`Opal_v0.12.bin` tuning, "Loading compatibility settings for IMX378" capability set), so streaming support
was probably not removed, just gated. Note the gate only runs for model 1: with our f63d program the onboarding check
skips the sensor test (then only needs a valid bcdDevice version), so Composer might now get further.

Flashing (the only persistent write Composer can do to a C1; main app only, device service only reads calibration):
- `checkForDeviceFlashingWithCompletion:` (model 1/2): sensor `IMX378` → not eligible; else opens
  `DeviceBootloader(info, allowFlashingBootloader=true)` and is eligible ONLY if `getFlashedVersion() == 0.0.15`
  (ours is 0.0.15). It never calls `flashBootloader`.
- In-use prompt ("New firmware… proceed with update right now?", log "Detected old firmware X. Offering update.") only
  when sensor == `IMX582` AND camera version < 4.10 → never for this camera.
- `flashOpalC1Device:` = `flashConfigClear` + `flash(pipeline, compress)` + `flashConfig` (default USB config, vid 03e7
  pid f63c). Pipeline (`getApplicationPipelineWithImageTunning:`): ColorCamera + on-camera PDAF autofocus (Kalman) +
  face-detection NN for focus, ImageManip driven by a Script that follows the UVC frame index (USB3: 4K/1440p/1080p/
  720p, USB2: 720p), UVC with GPIO 34 = 1 on stream on / 0 off, AudioMic (back mic, +28 dB). No sensor idle stop.
- Risk if run: the onboarding flash check uses a NEGATIVE test (≠ IMX378). If Composer's firmware fails to read the
  sensor name, the camera would look eligible and onboarding would offer "Tap to update" (needs a click). Worst case
  it replaces our standalone program (bootloader untouched) → recoverable with `tools/bin/opaltool flash idle`.
- Like Opal 0.26, its device service UVC-resets f63b/f63d cameras and host-boots its own firmware for app mode, so it
  fights our custom Opal for the camera; quit/boot out ours before running it.

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
  - `tools/opal-watch/stream-monitor.py [summary_secs]` — live watch for a test session (used for the overnight
    soak): events + temp/load summaries, auto-captures incident state on crash signatures.
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
