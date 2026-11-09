# Opal for Windows (work in progress)

A Windows app that drives the Opal C1 the way Opal.app does on the Mac. Nothing of Opal.app's code runs on
Windows: the camera side is the public Luxonis depthai-core plus Opal's own camera firmware and pipeline, captured
from the real app.

## How the camera side works

1. **Library**: `build-depthai.sh` builds public depthai-core at `a3c01e446` (branch `custom_uvc_uac_scripting`,
   2022-11-18), the commit Opal's private fork (`4386d6e1`, 2023-04-01) is based on: same bootloader (`bb9e4c86`,
   0.0.15) and the depthai-shared revision with `GlobalProperties.imageManipAdjustCmxSize`, which Opal's firmware
   requires. Patches applied by the script: Opal's firmware embedded (no download), zlib 1.3.1 (macOS SDK fix), and
   the payload copy in `StreamMessageParser::parseMessageToADatatype` restored (commented out as "TMPTMP" at that
   commit; without it every message arrives with empty data).
2. **Firmware**: `extract-opal-fw.py` pulls `depthai-device-fwp-f57b4691….tar.xz` and the bootloader package out
   of Opal.app's `libdepthai-core.dylib` into `tools/vendor/opal-fw/` (gitignored).
3. **Pipeline**: Opal's camera service was recorded with `tools/opalcore/pipecap.mm` (custom Mac build, enabled by
   `~/Library/Logs/opal-watch/pipecap/ENABLE`) while every feature was toggled. `make-template.py <session>` turns
   that into `tools/vendor/opal-template/{full,lowpower}.json` + `assets/` (NN blobs, on-camera scripts, IMX378
   tuning; gitignored). `core/opal_pipeline.cpp` rebuilds the pipeline from the template through the public API.
   Verified: all 33 nodes serialize byte-identical to what Opal sent (`smoke/verify.cpp`, `smoke/opalstream.cpp`).
4. **Result on the camera (macOS, 2026-11-01)**: the rebuilt pipeline boots and runs on the owner's C1 with all
   streams delivering data: `rgb` 1080p NV12 (3.1 MB/frame, ~26–30 fps), `face_detection`, `google_meet_segmentation`
   (147 KB mask), `handpose_python_manager`, `uac` (audio, 11.5 KB chunks), `sysinfo` (1/s).

## Opal's app-mode pipeline (captured)

ColorCamera (4K sensor, `video` scaled by ISP per resolution, `preview` 640×360 for the NNs, rotated 180°, AF off:
the host moves the lens, start position 126, limits 120–255) →
- `video` → XLinkOut `rgb` (the picture; host applies looks/bokeh/framing on the GPU)
- `preview` → ImageManip 300×300 → MobileNet DetectionNetwork (faces) → `face_detection`
- `preview` → ImageManip 256×144 → NeuralNetwork (Google Meet segmentation, bokeh mask) → `google_meet_segmentation`
- `preview` → hand-pose chain: ImageManip 128×128 → palm detection NN → post-processing NN → Script (on-camera
  Python manager) → ImageManip 256×256 → landmark NN → Script → `handpose_python_manager` (gestures)
- AudioMic 48 kHz → UAC (USB microphone, works natively on Windows) and XLinkOut `uac`
- Script (LED on/off via `ledCtrl`), SystemLogger 1 Hz → `sysinfo`

Inputs (XLinkIn): `control` (CameraControl), `config` (camera ImageManip config), `audioControl`, `ledCtrl`,
`ctrlFace`/`ctrlFaceImageManip`, `ctrlSegmenter`/`ctrlSegmenterImageManip`, `ctrlGestures`/
`ctrlGesturesPrePdImageManip`, `gesturesCtrlQueueChokeEmbedded`.

Variants: resolution changes restart the pipeline with only ColorCamera `ispScale`/`videoWidth`/`videoHeight` (and the
current lens position) changed: 4K none, 1440p 2/3, 1080p 1/2, 720p 1/3, 360p 1/6. Low-power mode is a separate
12-node pipeline (UVC output, no NNs).

## Camera findings folded in (2026-11-06, from the Mac investigation; details in tools/README.md "Camera profiler")

- Any camera control applying 2+ commands at once makes the camera drop every 8th frame for the session (26.4 instead
  of 30 fps). `OpalCamera::send` therefore sends camera controls one command per message, 40 ms apart, and the
  template's initial control is uploaded with an empty command mask (`sendInitialControls` sends the same settings).
- A freshly booted camera streams by default; `sendInitialControls` sends an explicit STOP_STREAM when streaming isn't
  wanted (Opal assumed "off" and left idle cameras streaming for hours).
- The gesture choke was already right here (sent only with the network state), so the hand-pose networks never run
  with gestures off. 4K raw is USB-bound (~24 fps, 229 ms) and 4K H.264/H.265 on the camera encoder costs CSS 59 % /
  MSS 72 %: keep 1080p (or 1440p) as the default.

## Control protocol (decoded from the capture: `smoke/decodectl.cpp`)

- Camera on: `ledCtrl` [1,1,1,1], `gesturesCtrlQueueChokeEmbedded` [1], CameraControl START_STREAM + lens, AE auto,
  AWB, scene, exposure compensation, control mode, saturation… Off: STOP_STREAM, LED [1,0,0,0].
- Each NN is switched by a pair: ImageManipConfig with `skipCurrentImage` true/false on its `…ImageManip` input and
  NNConfig `processInputs` false/true on its `ctrl…` input.
- Focus: face/host driven `MOVE_LENS` + `AF_MODE off`, or one-shot `AF_TRIGGER` with `AF_MODE auto`.
- Full timeline: `tools/vendor/opal-capture/session1/controls-summary.txt` (gitignored with the capture).

## Building on the Mac, installing on the PC (Windows 11 only) - the normal way

- On the Mac: `tools/win/build-windows-on-mac.sh` cross-compiles everything with MinGW-w64
  (`cmake/mingw-w64-x86_64.cmake`; depthai-core static with Opal's firmware embedded, GCC runtime linked statically, so
  the binaries only use DLLs that ship with Windows) -> `build/Opal-Windows-<date>.zip`.
- On the PC: unzip, run `Opal.exe`, click OK: it relaunches itself elevated (`--install <dir>`), copies itself to
  C:\Program Files\Opal, registers the virtual camera DLL (calls its DllRegisterServer) and starts the installed
  copy. Tray menu "Uninstall Opal..." reverses it (`--uninstall`, run from a temp copy). No PowerShell/.cmd installer:
  the first package had one and Chrome/Defender flagged the download as a virus (script with ExecutionPolicy
  Bypass + RunAs = dropper pattern). The exe also carries a manifest and version info for the same reason. The
  binaries are unsigned (no Windows code-signing certificate yet).
- MinGW quirks handled in the toolchain file: GCC 14 pointer-type errors in XLink's win_usb.c are downgraded, and
  `<stdexcept>/<utility>/<cstdint>` are force-included (depthai relies on libc++'s transitive includes).

## Alternative: building on the PC with MSVC

1. On the Mac: `tools/win/pack-for-windows.sh` -> `build/opal-windows-src.zip` (sources + Opal's assets; prereqs:
   `extract-opal-fw.py`, `make-template.py <pipecap session>`, `luts/bake-luts.sh`).
2. On the PC: install Visual Studio 2022 Build Tools ("Desktop development with C++", Windows 11 SDK), CMake, Git.
   Unzip, then `powershell -ExecutionPolicy Bypass -File tools\win\build-win.ps1` (first run builds depthai-core,
   10-20 min) -> `tools\win\dist\`.
3. Run `tools\win\dist\Opal.exe` and click OK (installs itself). Logs: `%APPDATA%\Opal\opal.log`.
   `OpalVCamTest.exe` (in dist) tests the virtual camera alone: quit Opal, run it, open the Camera app, pick
   "Opal C1": dark gray briefly, then a moving pattern; Enter removes it. Virtual camera logs: DebugView (admin,
   "Capture Global Win32"), filter `[OpalVCam`. `Start` errors: 0x80070005 = DLL folder not readable by services,
   0x80040154 = DLL not registered.

## Pieces

- `core/` (portable C++, also built on macOS by `smoke/`): `opal_pipeline` (template -> pipeline), `opal_camera`
  (find/boot/reconnect, sensor on/off held 5 s, network switching, camera controls, mic gain, LED, telemetry).
  Verified on the owner's camera on macOS with `smoke/camtest.cpp`.
- `app/`: tray app. `effects` (D3D11 compute: NV12->RGB like Opal (BT.709 full range), crop/zoom, mirror, bokeh /
  pixellate / spotlight with the camera's person mask, touch-up, skin, highlights/shadows, look LUT with strength,
  overlay, -> NV12 BT.709 video range for Windows apps), `overlay` (Direct2D: text, logo, gesture drawing, peace
  burst), `framing` (zoom, face lock), `gestures` (draw / peace / pinch from hand landmarks by finger geometry;
  Opal uses a random-forest model on the Mac), `vcam_producer`, `ui` (popover: Camera, Effects, Manual, Audio,
  Settings, footer with status; a status readout over the live preview like the Mac app's statushud: chip °C, camera
  CPU CSS/MSS %, received fps + frame height, Opal.exe CPU as % of one core, details on hover; orange ≥ 65 °C, red ≥ 75 °C), `fluent` (Windows 11 / WinUI 3 controls drawn with Dear ImGui to
  WinUI's metrics and colour tokens: ToggleSwitch, Slider, ComboBox, Button, CheckBox, TextBox, SelectorBar, settings
  cards, ScrollViewer with smooth wheel scrolling; follows the system light/dark theme and accent colour; Segoe UI via
  FreeType; icons are Microsoft's Fluent UI System Icons, MIT, subset into `app/icons/` by `mkicons.py`),
  `settings` (%APPDATA%\Opal\settings.json). The popover is a frameless WS_THICKFRAME window (DWM shadow, rounded
  corners).
- `preview/preview.sh [outdir]`: renders the popover on the Mac (same `ui.cpp`/`fluent.cpp`, Metal backend, Tahoma
  standing in for Segoe UI, placeholder picture) to PNGs, to check the design without the PC.
- Microphone mute = Opal's: `AudioInConfig.disableOutput` on the `audioControl` stream (captured from Opal: micGain
  31.6, disableOutput 1), persisted as `micMuted`; also in the tray menu.
- `vcam/`: `OpalVirtualCamera.dll` (Media Foundation virtual camera source for the Frame Server) + `OpalVCamTest.exe`.
- `luts/bake-luts.sh`: bakes Opal's 13 looks (8 Core Image + 5 Metal shaders, run from Opal's own code) into 64³
  `.cube` LUTs; all are per-pixel. Strength = `in + (lut(in) - in) * s`.
- Checked here: all Windows code compiles and links with MinGW-w64 against a stub depthai (`CMAKE_SYSTEM_NAME=Windows`
  cross build), shaders validate with glslang. NOT yet run on Windows.

## Status / next

- [x] firmware extraction, library build (macOS), pipeline capture + template, byte-identical rebuild, streams on camera
- [x] camera core (tested on the camera on macOS), effects, overlays, framing, gestures, UI, virtual camera, build +
      install scripts (compile-checked only)
- [ ] first run on Windows 11; low-power mode (Opal's UVC pipeline) not wired up yet

## Smoke tools (macOS)

```bash
tools/win/extract-opal-fw.py && tools/win/build-depthai.sh
CMAKE_POLICY_VERSION_MINIMUM=3.5 cmake -S tools/win/smoke -B tools/vendor/smoke-build-mac -G Ninja \
  -Ddepthai_DIR=$PWD/tools/vendor/depthai-build-mac/install/lib/cmake/depthai && cmake --build tools/vendor/smoke-build-mac
# quit Opal + boot out its USB agent first (see AGENTS.md), then:
tools/vendor/smoke-build-mac/opalstream tools/vendor/opal-template 10
```

Note: a hand-made ColorCamera.video → XLinkOut pipeline crashes Opal's firmware (`Fatal error on MSS CPU: trap 00`)
about 1.5 s after the sensor starts, with Opal's own library too; Opal's real pipeline does not. Cause not known yet.
