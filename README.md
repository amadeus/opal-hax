# opal-hax

A rescue project for the **Opal C1** webcam. Its maker abandoned it, and both
the camera and its Mac app have problems that will never be fixed upstream. This
repo turns Opal's last Mac app (v0.26, build 25.992) into a custom build under
my own Apple developer account, adds newer camera firmware, and has a Windows
app that drives the camera the same way. It also has the reverse-engineering
notes and camera tools behind all of it.

Unofficial and unsupported. It works on my camera and my Mac and PC. Flashing
firmware can brick a camera, so use it at your own risk.

## What it fixes

**A cooler, more reliable camera**
- **The camera only works when something is using it.** Stock, the sensor and
  the on-camera neural networks kept running whenever the camera was plugged in,
  even with nothing watching, so it sat there getting hot. Over long sessions it
  would "get stuck": it dropped out as a camera and only unplugging helped. Now
  the camera idles properly when nothing is using it, and the hand-gesture
  networks only run when gestures are turned on, which was most of the heat in
  long calls.
- **A full 30 fps** at 1080p (stock delivered about 26).
- **Lots of smaller fixes**: no on/off flapping when the popover opens and
  closes, mute that stays muted, the camera turning off reliably when a call
  ends, and more.

**Logging for future problems**
- The app records the camera's own logs, its chip temperature, CPU load and
  memory, and every stream on/off decision in `~/Library/Logs/opal-watch/`. If
  the camera misbehaves, there's a record of what led up to it.

**A tidier app**
- The popover only opens when you click the menu-bar icon, and a small readout
  over the preview shows the camera's temperature, load and frame rate.
- Polish: the looks thumbnails play live, cleaner corners and spacing, and other
  small UI fixes.
- Removed: all the built-in updating (the app is abandoned, and the updater
  could re-flash the camera), the feedback form, the noise cancellation panel,
  and the Sentry and Mixpanel trackers.

## Installing on a Mac

1. Unzip `Opal-0.26-custom.<timestamp>.pkg.zip` from the repo root and
   double-click the `.pkg`. It is signed and notarized, so it runs on any Mac.
2. The installer replaces any original Opal (the old app goes to the Trash),
   installs `/Applications/Opal.app`, activates the camera extension and starts
   Opal. It keeps that Mac's Opal settings, or uses the defaults.
3. **First install only:** approve the camera under System Settings → General →
   Login Items & Extensions → Extensions → Camera Extensions ⓘ → turn on **Opal
   C1**. Opal walks you through it.

Install log: `/var/log/opal-custom-install.log`.

## Opal for Windows

A Windows 11 app that drives the Opal C1 the way Opal does on the Mac. None of
Opal's Mac code runs on Windows: it uses the public Luxonis camera library with
Opal's own camera firmware and pipeline, captured from the real app. It has the
looks, background blur, touch-up, framing, gestures and microphone settings, a
tray icon with a Windows 11-style panel, and an **Opal C1** virtual camera for
video apps.

To install: unzip `Opal-Windows-<date>.zip` from the repo root, run
`InstallOpal.exe` and click Install. It installs itself in Program Files, adds
the virtual camera, and can add Start menu and desktop shortcuts. Uninstall from
the tray menu. The binaries aren't code-signed, so Windows may warn the first
time.

The Windows app is newer and less tested than the Mac build. Details are in
`tools/win/README.md`.

## Camera firmware

The camera is an Intel Movidius Myriad X with a Sony IMX378 sensor. Its firmware
is a closed binary, but the *pipeline* it runs is ours to change. On my camera:

- **Bootloader** 0.0.4 → **0.0.15**, the one bundled in Opal.app. The app takes
  over the camera faster.
- **Standalone webcam program**, used when no Opal app is running: Opal's own
  pipeline (4K sensor scaled to 1080p, autofocus, anti-banding, mic) **plus an
  idle watcher** that stops the sensor 3 s after the last app stops using the
  camera. It shows up as USB `03e7:f63d`.

The Mac app offers this update when it sees a camera still on Opal's original
firmware, behind an explicit brick-risk warning ("Not Now" is the default). The
updater is **untested**: my camera was updated by hand with
`tools/bin/opaltool`.

## Building from source

You need the original Opal 0.26 app at the repo root as `Opal Backup.app`
(proprietary, not in the repo) and the Xcode command-line tools.

**Mac app.** Also needs an Apple developer team with **Developer ID
Application** and **Developer ID Installer** certificates, a Developer ID
provisioning profile for the Setup app (`com.amadeus.oc.OpalExtensionInstaller`,
which needs the system-extension entitlement), and notarytool credentials in the
keychain profile `opal-notary`.

```bash
tools/fetch-deps.sh                      # once: public depthai headers for the camera tools
python3 tools/custom-build/build_custom_opal.py \
    --developer-id --profile build/Setup-DeveloperID.provisionprofile   # → build/Opal.app
tools/custom-build/notarize.sh           # notarize + staple build/Opal.app
tools/custom-build/make-pkg.sh           # → build/Opal-0.26-custom.<timestamp>.pkg (signed, notarized)
```

Team id, bundle ids and signing identities are hard-coded for my account in
`build_custom_opal.py` and `make-pkg.sh`. Change them before building under
another team.

**Windows app.** Cross-compiled on the Mac with MinGW-w64 (`brew install
mingw-w64 ninja cmake`). The one-time setup (camera library, firmware
extraction, pipeline template) is in `tools/win/README.md`, then:

```bash
tools/win/build-windows-on-mac.sh        # → build/Opal-Windows-<date>.zip
```

## Repo layout

| Path | What |
|---|---|
| `AGENTS.md` | The full investigation log: hardware, app internals, every finding and decision, and a hang playbook. Start here for details. |
| `Opal-*.pkg.zip`, `Opal-Windows-*.zip` | Latest Mac installer and Windows app. |
| `tools/custom-build/` | Builds, signs, notarizes and packages the custom Mac app. |
| `tools/opalspy/` | Code built into the Mac app: telemetry, UI changes, status readout, firmware offer, extension setup guide. |
| `tools/opalcore/` | Code built into the Mac camera service: camera logs, streaming and idle fixes, mute, frame-rate fix. |
| `tools/win/` | The Windows app, its virtual camera, and the Mac-side build. |
| `tools/camera-updater/` | "Opal Camera Updater.app", the firmware flasher the Mac app launches. |
| `tools/opaltool.cpp`, `standalone.hpp` | Command-line camera tool: host-boot tests, flashing, bootloader version. The idle-watcher pipeline lives in `standalone.hpp`. |
| `tools/profile/` | Camera profiler: what each part of Opal's pipeline costs on the camera. |
| `tools/opal-watch/` | Background recorder for USB events, Opal logs and crashes, plus incident capture for hangs. |

`tools/README.md` covers the camera tools and their commands.

## Status

- The custom Mac build is in daily use. The original "camera gets stuck" hang
  hasn't come back since the changes.
- If the camera drops out, check the logs before blaming it: a flaky USB device
  on the same dock can knock it off too. The hang playbook in `AGENTS.md` walks
  through it.

Opal, Opal C1 and the Opal app belong to their original makers. The original app
isn't in this repo; the installer zip contains a modified, re-signed build of
it.
