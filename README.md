# opal-hax

A rescue project for the **Opal C1** webcam. Its maker abandoned it, and both the camera and its Mac app have
problems that will never be fixed upstream. This repo has the reverse-engineering notes, camera tools, and build
scripts that turn Opal's last Mac app (v0.26, build 25.992) into a custom build under my own Apple developer
account, plus newer camera firmware.

## The point

1. **The sensor stays off when nothing is using the camera.** Stock, the sensor keeps running whenever the
   camera is plugged in without the app, or whenever the app's popover is open, so the camera just sits there
   getting hot. The new firmware turns the sensor off 3 s after the last app stops using the camera. The custom
   app stops the stream reliably and without flapping. Over long sessions the stock camera would get hot and
   "get stuck": it dropped out as a camera and only unplugging helped.
2. **Built-in logging to debug future issues.** The app records:
   - the camera's own logs
   - chip temperature, CPU load, and memory
   - every stream on/off decision

   It writes them to `~/Library/Logs/opal-watch/`, so if the camera hangs again there's a record of what led up to
   it.
3. **Polish and UX tweaks to the app:**
   - It no longer pops open (and starts the camera) on launch.
   - All the built-in updating is gone. The app is abandoned, so the update checks were useless, and they could
     re-flash the camera.
   - The Noise Cancellation section is gone. It's a feature that never shipped in this version and just took up
     space.
   - The Sentry and Mixpanel trackers are blocked.

Unofficial and unsupported. It works on my camera and my Macs. Flashing firmware can brick a camera, so use it at
your own risk.

## What's in the custom build

The custom build is Opal 0.26 re-signed under team `UHD99KF9X7`, with bundle ids `com.amadeus.oc.*`. It replaces the
original app.

- **Streaming debounce.** "Stream off" waits 5 s and is cancelled by any newer "on", and the sensor is only
  stopped if no app is still using the Opal C1 camera. This stops the rapid on/off flapping.
- **Quiet launch.** No popover and no stream when Opal starts.
- **No trackers.** Sentry and Mixpanel hosts are swapped for `.invalid`, and the build checks that none remain.
- **No updating.** Update checks, downloads and installs, the firmware-update prompt, and all of the app's flash
  routines are patched to return immediately. The Updates panel is hidden, and the Settings page is resized to
  fit.
- **Noise cancellation panel hidden**, and the Audio page resized to fit.
- **Camera logs and telemetry.** Chip temperature, CPU load, and memory are written to
  `~/Library/Logs/opal-watch/` (`camera.log`, `telemetry.log`). This is how hangs get diagnosed.
- **Auto-launch with the new firmware.** Opal starts when the camera is plugged in, on either USB id: stock
  firmware (`f63b`) or ours (`f63d`).
- **Setup help for the camera extension.** macOS hides the switch for approving the extension. If it still needs
  approval, Opal shows step-by-step instructions and a button that opens the right Settings pane.
- **Camera firmware updater.** It appears when a camera still runs Opal's original firmware (see below).

Release builds are signed with Developer ID and notarized, so they run on any Mac.

## Camera firmware

The camera is an Intel Movidius Myriad X with a Sony IMX378 sensor, running a private fork of Luxonis
depthai-core 2.15.1. The device firmware is a closed binary, but the *pipeline* it runs is ours to change.

The firmware on my camera:

- **Bootloader** 0.0.4 → **0.0.15**, the one bundled in Opal.app. Camera takeover by the app is faster and skips
  the Myriad USB ROM step.
- **Standalone webcam program** (used when no Opal app is running): Opal's own pipeline (4K sensor scaled to 1080p
  UVC, 180° rotation, autofocus, anti-banding, mic +28 dB) **plus an idle watcher**. It stops the sensor 3 s after
  the last app stops using the camera and restarts it when one connects, so the camera stays cool. It enumerates
  as USB `03e7:f63d`.

The custom app offers this update when it sees a camera on Opal's original firmware. The update is behind an
explicit brick-risk warning, and "Not Now" is the default. The update itself:

1. Flashes the bootloader only if the camera's is older.
2. Flashes the idle-watcher program, with 50 or 60 Hz anti-banding picked from the Mac's region.
3. Restarts Opal.

The updater shipped **untested**. My camera was already updated by hand with `tools/bin/opaltool`.

## Installing on a Mac

1. Unzip `Opal-0.26-custom.<timestamp>.pkg.zip` from the repo root and double-click the `.pkg`. It is signed and
   notarized.
2. The installer:
   - quits and removes any original Opal: the app goes to the Trash, and its camera extension, launchd items,
     helper tools, legacy DAL plug-in, and install receipt are removed
   - installs `/Applications/Opal.app` with its launchd items
   - activates the camera extension and starts Opal
   - keeps that Mac's existing Opal settings, or uses the defaults; settings are never shipped in the package
3. **First install only:** approve the camera under System Settings → General → Login Items & Extensions →
   Extensions → Camera Extensions ⓘ → turn on **Opal C1**. Opal walks you through it.
4. **Restart after upgrading from an older build.** When a new build replaces the camera extension, macOS only
   starts the new extension after a restart, and Opal says so.

Install log: `/var/log/opal-custom-install.log`.

## Building from source

You need:

- the original Opal 0.26 app at the repo root as `Opal Backup.app`. It is proprietary and not in the repo.
- Xcode command-line tools
- an Apple developer team with **Developer ID Application** and **Developer ID Installer** certificates
- a Developer ID provisioning profile for the Setup app (`com.amadeus.oc.OpalExtensionInstaller`), which needs
  the system-extension entitlement
- notarytool credentials saved under the keychain profile `opal-notary`

```bash
tools/fetch-deps.sh                      # once: public depthai headers for the camera tools
python3 tools/custom-build/build_custom_opal.py \
    --developer-id --profile build/Setup-DeveloperID.provisionprofile   # → build/Opal.app
tools/custom-build/notarize.sh           # notarize + staple build/Opal.app
tools/custom-build/make-pkg.sh           # → build/Opal-0.26-custom.<timestamp>.pkg (signed, notarized)
```

To update this Mac in place without the package, run `tools/custom-build/update-custom-opal.sh`. It uses sudo and
keeps the replaced build in `previous-custom/`.

Team id, bundle ids, and signing identities are hard-coded for my account in `build_custom_opal.py` and
`make-pkg.sh`. Change them before building under another team.

## Repo layout

| Path | What |
|---|---|
| `AGENTS.md` | The full investigation log: hardware, app internals, every finding and decision, and a hang playbook. Start here for details. |
| `Opal-*.pkg.zip` | Latest installer. |
| `tools/custom-build/` | Builds, signs, notarizes, and packages the custom app (`build_custom_opal.py`, `notarize.sh`, `make-pkg.sh`, `pkg/` install scripts). |
| `tools/opalspy/` | Code injected into the main app: telemetry, debounce, quiet launch, hidden panels, firmware offer, extension setup guide. |
| `tools/opalcore/` | Code injected into the camera-driver service: debounce at the source, camera log tap, chip temperature. |
| `tools/camera-updater/` | "Opal Camera Updater.app", the firmware/bootloader flasher the app launches. |
| `tools/opaltool.cpp`, `standalone.hpp` | Command-line tool for the camera: host-boot tests, flashing, bootloader version. The idle-watcher pipeline lives in `standalone.hpp`. |
| `tools/probe.cpp`, `thermal.cpp` | Passive device listing; temperature experiments. |
| `tools/opal-watch/` | Background recorder for USB events, Opal logs, and crashes, plus incident capture and reports for hangs. |
| `tools/opal-launch/` | Older approach: launch the unmodified app with `opalspy` injected. Superseded by the custom build. |

`tools/README.md` covers the camera tools and their commands.

## Status

- The custom build is installed and working on two Macs (2026-11-01).
- An overnight 4K stream ran 4 h 22 min with no hang. Chip temperature plateaued around 59–61 °C.
- The original hang hasn't been reproduced since the changes. If it happens again, follow the hang playbook in
  `AGENTS.md`.

Opal, Opal C1, and the Opal app belong to their original makers. The original app isn't in this repo; the
installer zip contains a modified, re-signed build of it.
