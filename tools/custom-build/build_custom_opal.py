#!/usr/bin/env python3
"""Build a custom, self-signed Opal.app from the pristine original ("Opal Backup.app").

  /usr/bin/python3 -I tools/custom-build/build_custom_opal.py [--profile path/to/Setup.provisionprofile]

Output: build/Opal.app (never touches /Applications or the pristine source).

Steps
 1. fresh copy of the source (ditto, no xattrs)
 2. strip every code signature, _CodeSignature dir and embedded.provisionprofile
 3. same-length byte replacements in every file (Swift/CFString literals carry lengths, so lengths must match):
      group.97Z3HJWCRT.com.opalcamera.Opal -> UHD99KF9X7.com.amadeus.oc.OpalShared  (team-prefixed group: no profile)
      97Z3HJWCRT                           -> UHD99KF9X7                            (team id)
      com.opalcamera                       -> com.amadeus.oc                        (bundle ids, mach services, subsystems)
      opalcamera.sfo3.digitaloceanspaces.com -> xxx….invalid                        (auto-updater can't replace our build)
      o815788.ingest.sentry.io / api.mixpanel.com -> xxx….invalid                   (no crash reports / analytics)
 4. rename files/dirs whose names contain com.opalcamera
 4b. no updating: main executable thinned to arm64 and its update/firmware-flash entry points patched to return
     (CODE_PATCHES, original bytes verified first); the updater XPC service is removed. The Updates settings
     panel is hidden at runtime by opalspy (tools/opalspy/hidepanels.m), as is Audio > Noise cancellation.
 5. build our code in: opalcore.dylib -> camera-driver XPC service, opalspy.dylib -> main app (LC_LOAD_DYLIB)
 6. entitlements per component = originals with the same replacements, minus team-restricted keys
    (application-identifier, team-identifier, associated-domains). The Setup app (system-extension host) keeps
    system-extension.install only when --profile is given (it needs a provisioning profile for that).
 7. sign inside-out with the Apple Development identity (hardened runtime), then verify.
"""
import argparse
import os
import plistlib
import tempfile
import re
import struct
import shutil
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
TOOLS = os.path.join(ROOT, "tools")
SRC = os.path.join(ROOT, "Opal Backup.app")
OUT = os.path.join(ROOT, "build", "Opal.app")
IDENTITY = "Apple Development: Amadeus Demarzi (J972UZ26TC)"
# --developer-id: sign for distribution (any Mac, notarizable, secure timestamp so builds outlive the certificate)
DEVID_IDENTITY = "Developer ID Application: Amadeus Demarzi (UHD99KF9X7)"
TIMESTAMP = "--timestamp=none"
TEAM_OLD, TEAM_NEW = b"97Z3HJWCRT", b"UHD99KF9X7"

REPLACEMENTS = [
    (b"group.97Z3HJWCRT.com.opalcamera.Opal", b"UHD99KF9X7.com.amadeus.oc.OpalShared"),
    (TEAM_OLD, TEAM_NEW),
    (b"com.opalcamera", b"com.amadeus.oc"),
    (b"opalcamera.sfo3.digitaloceanspaces.com", b"x" * 30 + b".invalid"),
    # tracking off: Sentry crash/breadcrumb uploads (DSN host in Opal + both XPC services) and Mixpanel analytics
    # (events incl. the camera serial). Unresolvable hosts, so nothing leaves the Mac.
    (b"o815788.ingest.sentry.io", b"x" * 16 + b".invalid"),
    (b"api.mixpanel.com", b"x" * 8 + b".invalid"),
]
TRACKERS = [b"ingest.sentry.io", b"api.mixpanel.com"]
for a, b in REPLACEMENTS:
    assert len(a) == len(b), (a, b)

# Updating is removed: the app is ours now. Each entry: (VA in Opal 0.26 arm64, expected original bytes, new bytes,
# what). RET = `ret`; RET0 = `mov x0, #0; ret`. All are void functions/methods (or BOOL for RET0) whose callers
# ignore the result, so returning on entry is clean.
RET, RET0 = bytes.fromhex("c0035fd6"), bytes.fromhex("000080d2c0035fd6")
NOP = bytes.fromhex("1f2003d5")
CODE_PATCHES = [
    (0x10024fb44, "fc6fbda9f44f01a9", RET, "UpdateManager.scheduleCheckForUpdate()"),
    (0x100252478, "fc6fbda9f44f01a9", RET, "UpdateManager.checkForUpdate()"),
    (0x100254db8, "fc6fbda9f44f01a9", RET, "UpdateManager.downloadUpdate(...)"),
    (0x100257bb0, "ff0307d1f44f1aa9", RET, "UpdateManager.launchUpdateInstallation (runs the .pkg via OpalCameraUpdateXPC)"),
    (0x1003ab9a0, "f44fbea9fd7b01a9", RET, "FirmwareUpdateManager: 'New Firmware Update' prompt"),
    (0x1003a8958, "f44fbea9fd7b01a9", RET, "FirmwareUpdateManager: flash closure"),
    (0x1000143f4, "ffc301d1f65704a9", RET, "-[FirmwareManager flashBootloaderWithProgress:andCompletion:]"),
    (0x10001493c, "ffc301d1f65704a9", RET, "-[FirmwareManager flashApplicationPipelineWithProgress:andCompletion:]"),
    (0x10001541c, "f657bda9f44f01a9", RET0, "-[FirmwareManager updateCalibrationVersion] (writes EEPROM)"),
    (0x1001c2640, "ff8301d1f44f04a9", RET, "-[DeveloperModeViewController forceFlashCamera:]"),
    # TabManager page height: the Audio page is a fixed 3 rows (Mute, Studio Sound, Noise cancellation) of 72 pt + 10 pt
    # gaps; Noise cancellation is hidden (opalspy hidepanels.m), so 2 rows.
    (0x1003b56fc, "0010611e", bytes.fromhex("0010601e"), "Audio page rows: fmov d0, #3.0 -> #2.0"),
    # The popover opens only when the menu-bar icon is clicked (owner, 2026-11-04). Opal opens it by itself on launch
    # and when the camera connects, and an open popover keeps the camera streaming. The launch call
    # ([appDelegate togglePopover:nil]) also sets Opal up (loads the camera view controller; without it the camera
    # doesn't connect until the first click), so it stays and opalspy's launchsetup.m runs it without the display
    # step. The camera-connect calls are removed (NOP; each is a void call whose next instruction reloads its
    # registers from the stack). The click action togglePopover: (status item button) is untouched.
    # Clicking the icon with the popover open closed it and reopened it at once: on this macOS a click on the app's
    # own status item first deactivates the app, Opal's applicationWillResignActive closes the popover, and the click's
    # togglePopover: then finds it closed and shows it. Opal sets popover.animates = false, which makes that close
    # instant; with AppKit's default animation the popover is still closing (isShown) when the action arrives, so the
    # toggle closes it and it stays closed (reproduced both ways with a stand-in status item app, 2026-11-04).
    (0x100386a48, "08008052", bytes.fromhex("28008052"), "popover.animates = false -> true (mov w8, #0 -> #1)"),
    (0x1001ca9f8, "196afa97", NOP, "camera connect: showPopover() in DeviceXPCConnectionHelperService+OpalCameraInputDelegate"),
    (0x1001cb1b0, "2b68fa97", NOP, "camera connect: showPopover() (abortedResizingFrame(withFallbackResolution:))"),
    (0x1001cd1e4, "1e60fa97", NOP, "camera connect: showPopover() in DeviceXPCConnectionHelperService+OpalCameraInputDelegate"),
    (0x1001ce91c, "505afa97", NOP, "camera connect: showPopover() in DeviceXPCConnectionHelperService+OpalCameraInputDelegate"),
    (0x1002d7748, "c536f697", NOP, "camera frames: showPopover() in DeviceXPCConnectionHelperService's frame handler"),
    # Feedback removed (owner, 2026-11-09: the app is abandoned). Feedback mode (SendFeedbackView form that submits a
    # Sentry "User Feedback" + header + "email hello@opalcamera.com" footer) is shown while Storage.isFeedbackViewVisible
    # (@Published Bool) is true. Every writer (the Send Feedback buttons, the header's back toggle, the form's
    # done-sending close, Binding setters) goes through this one setter, so it now always stores false: feedback mode
    # can't open. The buttons themselves become Quit buttons and the feedback views EmptyView (opalspy hidepanels.m).
    (0x100324a10, "0800080a", bytes.fromhex("08008052"), "Storage.isFeedbackViewVisible setter: and w8, w0, w8 -> mov w8, #0 (always false)"),
    # Look thumbnails (FilterImageView.body): the tile is clipped with .cornerRadius(10) over a .background of the same
    # RoundedRectangle filled with the label colour (white, the accent yellow when selected, grey with looks off), so the
    # two anti-aliased curves overlap and the label colour leaks out around the picture (a white fringe, worst on the
    # dark placeholder). The selected tile hides it with its 1 pt .overlay stroke in the label colour; unselected tiles
    # stroked with Color.clear. Now they stroke with their own label colour too: the clear branch loads the background
    # Color built earlier in body ([x19+0x190], still alive inside the background view), retains it (the stroke's copy
    # is released after the call like Color.clear's was) and stores it; the "b next" it replaces was a no-op.
    # (A colour matching the card can't hide it: the card is translucent over the popover's material.)
    (0x10025b40c, "1ba70894604200f901000014", bytes.fromhex("60ca40f947b00894604200f9"),
     "FilterImageView.body overlay stroke: Color.clear -> swift_retain(background Color)"),
]
# Settings page content height (static Double used by SettingsView and TabManager's popover sizing). The four
# remaining panels measure 444 pt (96+126+126+66 + 3x10 spacing); 547 included the Updates panel. The Quit footer
# already starts ~10.7 pt below this area, so 444 leaves the same gap above Quit as between panel groups.
# Popover chrome height with a tab open (preview + tab bar 344 pt, + the 45 pt Quit footer; used by TabManager's
# popover size and every tab view's frame). The footer's button sits 10 pt below the divider but 13 pt above the
# bottom edge (measured on a 2x screenshot, 2026-11-09); 385.5 makes the bottom gap 1 px (0.5 pt) smaller than the top
# one, which looks even next to the window's inner border (owner's request).
DATA_PATCHES = [
    (0x100573b50, 547.0, 444.0, "settings page height"),
    (0x100573b48, 389.0, 385.5, "popover chrome height with a tab open (Quit footer bottom gap 13 -> 9.5 pt)"),
    # Looks thumbnails (FilterImageView): Opal asks its video service for 30 fps while the pointer is over a thumbnail and
    # 1 fps when it leaves (Int statics read by the onHover closure). 30 for both; opalspy filterfps.m starts every look
    # at 30 too (owner, 2026-11-09: "updates like once per second").
    (0x100575868, 1, 30, "Looks thumbnail fps when the pointer leaves it (1 -> 30)"),
]
TEXT_VMADDR = 0x100000000
# Camera extension build number. macOS only replaces an installed system extension when its version changes, so
# bump this whenever the extension's signature must be refreshed (e.g. 11: Developer ID re-sign, 2026-11-01).
EXT_BUNDLE_VERSION = "11"  # __TEXT vmaddr; fileoff 0 in the thin arm64 executable

RESTRICTED = {"com.apple.application-identifier", "com.apple.developer.team-identifier",
              "com.apple.developer.associated-domains"}
SETUP_REL = "Contents/MacOS/Opal Camera Setup.app"


def run(*cmd, check=True, quiet=False):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if check and r.returncode != 0:
        sys.exit(f"FAILED: {' '.join(cmd)}\n{r.stdout}{r.stderr}")
    if not quiet and r.stdout.strip():
        print(r.stdout.strip())
    return r


def is_macho(path):
    try:
        with open(path, "rb") as f:
            m = f.read(4)
    except OSError:
        return False
    return m in (b"\xca\xfe\xba\xbe", b"\xcf\xfa\xed\xfe", b"\xce\xfa\xed\xfe", b"\xbe\xba\xfe\xca")


def walk_files(top):
    for d, dirs, files in os.walk(top):
        for f in files:
            p = os.path.join(d, f)
            if not os.path.islink(p):
                yield p


def original_entitlements(rel_bundle):
    r = subprocess.run(["codesign", "-d", "--entitlements", "-", "--xml", os.path.join(SRC, rel_bundle)],
                       capture_output=True)
    if not r.stdout.strip():
        return {}
    data = r.stdout
    for a, b in REPLACEMENTS:
        data = data.replace(a, b)
    return plistlib.loads(data)


ENT_DIR = tempfile.mkdtemp(prefix="opal-entitlements-")


def sign(path, entitlements=None):
    cmd = ["codesign", "--force", "--sign", IDENTITY, "--options", "runtime", TIMESTAMP]
    if entitlements:
        ep = path.rstrip("/") + ".entitlements.plist"
        ep = os.path.join(ENT_DIR, os.path.basename(ep).replace(" ", "_"))  # scratch: not left in build/
        with open(ep, "wb") as f:
            plistlib.dump(entitlements, f)
        cmd += ["--entitlements", ep]
    run(*cmd, path, quiet=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--profile", help="provisioning profile for com.amadeus.oc.OpalExtensionInstaller (System Extension)")
    ap.add_argument("--developer-id", action="store_true",
                    help=f"sign with '{DEVID_IDENTITY}' + secure timestamp (use a Developer ID profile with --profile)")
    args = ap.parse_args()
    if args.developer_id:
        global IDENTITY, TIMESTAMP
        IDENTITY, TIMESTAMP = DEVID_IDENTITY, "--timestamp"

    # 1. fresh copy
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    if os.path.exists(OUT):
        shutil.rmtree(OUT)
    run("ditto", "--noextattr", "--noqtn", SRC, OUT)
    print("copied", SRC, "->", OUT)

    # 2. strip signatures
    for p in walk_files(OUT):
        if is_macho(p):
            run("codesign", "--remove-signature", p, check=False, quiet=True)
    for d, dirs, files in os.walk(OUT, topdown=False):
        for x in dirs:
            if x == "_CodeSignature":
                shutil.rmtree(os.path.join(d, x))
        for f in files:
            if f == "embedded.provisionprofile":
                os.remove(os.path.join(d, f))

    # 3. replacements
    total = 0
    for p in walk_files(OUT):
        with open(p, "rb") as f:
            data = f.read()
        new = data
        for a, b in REPLACEMENTS:
            new = new.replace(a, b)
        if new != data:
            n = sum(data.count(a) for a, _ in REPLACEMENTS)
            total += n
            with open(p, "wb") as f:
                f.write(new)
    print(f"replaced {total} identifier occurrences")

    # 4. rename paths
    for d, dirs, files in os.walk(OUT, topdown=False):
        for x in dirs + files:
            if "com.opalcamera" in x:
                os.rename(os.path.join(d, x), os.path.join(d, x.replace("com.opalcamera", "com.amadeus.oc")))
    leftovers = [p for p in walk_files(OUT) if b"com.opalcamera" in open(p, "rb").read() or TEAM_OLD in open(p, "rb").read()]
    assert not leftovers, f"identifiers left in: {leftovers}"
    trackers = [p for p in walk_files(OUT) if any(t in open(p, "rb").read() for t in TRACKERS)]
    assert not trackers, f"tracker endpoints left in: {trackers}"

    # 4b. no updating
    exe = os.path.join(OUT, "Contents/MacOS/Opal")
    run("lipo", exe, "-thin", "arm64", "-output", exe + ".arm64", quiet=True)
    os.replace(exe + ".arm64", exe)
    with open(exe, "r+b") as f:
        for va, orig, new, what in CODE_PATCHES:
            f.seek(va - TEXT_VMADDR)
            cur = f.read(len(bytes.fromhex(orig)))
            assert cur == bytes.fromhex(orig), f"unexpected code at {va:#x} ({what}): {cur.hex()}"
            f.seek(va - TEXT_VMADDR)
            f.write(new)
        segs = re.findall(r"segname (\S+)\n\s+vmaddr (0x[0-9a-f]+)\n\s+vmsize (0x[0-9a-f]+)\n\s+fileoff (\d+)",
                          run("otool", "-l", exe, quiet=True).stdout)
        for va, orig, new, what in DATA_PATCHES:
            seg = [(int(v, 16), int(o)) for n, v, sz, o in segs if int(v, 16) <= va < int(v, 16) + int(sz, 16)][0]
            f.seek(va - seg[0] + seg[1])
            fmt = "<q" if isinstance(orig, int) else "<d"  # Swift Int or Double/CGFloat
            cur = struct.unpack(fmt, f.read(8))[0]
            assert cur == orig, f"unexpected value at {va:#x} ({what}): {cur}"
            f.seek(va - seg[0] + seg[1])
            f.write(struct.pack(fmt, new))
    shutil.rmtree(os.path.join(OUT, "Contents/XPCServices/OpalCameraUpdateXPC.xpc"))
    print(f"updating removed: {len(CODE_PATCHES)} code patches, OpalCameraUpdateXPC.xpc deleted")

    ext_plist = os.path.join(OUT, SETUP_REL, "Contents/Library/SystemExtensions",
                             "com.amadeus.oc.OpalCamera.opalCameraExtension.systemextension/Contents/Info.plist")
    with open(ext_plist, "rb") as f:
        info = plistlib.load(f)
    info["CFBundleVersion"] = EXT_BUNDLE_VERSION
    with open(ext_plist, "wb") as f:
        plistlib.dump(info, f)
    print("camera extension version", info["CFBundleShortVersionString"], "build", EXT_BUNDLE_VERSION)

    # 5. our code
    run(os.path.join(TOOLS, "opalcore", "build.sh"), quiet=True)
    run(os.path.join(TOOLS, "opalspy", "build.sh"), quiet=True)
    dev = os.path.join(OUT, "Contents/XPCServices/OpalCameraDeviceService.xpc/Contents")
    os.makedirs(os.path.join(dev, "Frameworks"), exist_ok=True)
    shutil.copy(os.path.join(TOOLS, "opalcore", "opalcore.dylib"), os.path.join(dev, "Frameworks", "opalcore.dylib"))
    run("/usr/bin/python3", "-I", os.path.join(TOOLS, "opalcore", "insert_dylib.py"),
        os.path.join(dev, "MacOS", "OpalCameraDeviceService"), "@executable_path/../Frameworks/opalcore.dylib", quiet=True)
    shutil.copy(os.path.join(TOOLS, "opalspy", "opalspy.dylib"), os.path.join(OUT, "Contents/Frameworks/opalspy.dylib"))
    run("/usr/bin/python3", "-I", os.path.join(TOOLS, "opalcore", "insert_dylib.py"),
        os.path.join(OUT, "Contents/MacOS/Opal"), "@executable_path/../Frameworks/opalspy.dylib", quiet=True)
    # camera firmware updater (offered by opalspy firmwareoffer.m for cameras still on Opal's original firmware)
    run(os.path.join(TOOLS, "camera-updater", "build.sh"), quiet=True)
    os.makedirs(os.path.join(OUT, "Contents/Helpers"), exist_ok=True)
    run("ditto", os.path.join(TOOLS, "camera-updater", "build", "Opal Camera Updater.app"),
        os.path.join(OUT, "Contents/Helpers/Opal Camera Updater.app"))
    print("built in opalcore (camera-driver service), opalspy (main app) and the camera updater")

    # 6+7. sign inside-out
    def ent(rel, keep_restricted=False):
        e = original_entitlements(rel)
        if not keep_restricted:
            for k in RESTRICTED:
                e.pop(k, None)
        return e

    # loose dylibs and frameworks everywhere first
    for d, dirs, files in os.walk(OUT, topdown=False):
        for x in dirs:
            if x.endswith(".framework"):
                sign(os.path.join(d, x))
        for f in files:
            if f.endswith(".dylib"):
                sign(os.path.join(d, f))
    sign(os.path.join(OUT, "Contents/Helpers/Opal Camera Updater.app"))
    for x in ["OpalCameraVideoService", "OpalCameraDeviceService", "OpalCameraUninstallXPC"]:
        rel = f"Contents/XPCServices/{x}.xpc"
        sign(os.path.join(OUT, rel), ent(rel))
    ext_rel_src = SETUP_REL + "/Contents/Library/SystemExtensions/com.opalcamera.OpalCamera.opalCameraExtension.systemextension"
    ext_rel_out = ext_rel_src.replace("com.opalcamera", "com.amadeus.oc")
    sign(os.path.join(OUT, ext_rel_out), ent(ext_rel_src))
    setup_ent = ent(SETUP_REL, keep_restricted=bool(args.profile))
    if args.profile:
        shutil.copy(args.profile, os.path.join(OUT, SETUP_REL, "Contents/embedded.provisionprofile"))
        setup_ent["com.apple.application-identifier"] = "UHD99KF9X7.com.amadeus.oc.OpalExtensionInstaller"
        setup_ent["com.apple.developer.team-identifier"] = "UHD99KF9X7"
    else:
        setup_ent.pop("com.apple.developer.system-extension.install", None)
        print("NOTE: no --profile given: Setup app signed WITHOUT system-extension.install (can't activate the extension)")
    sign(os.path.join(OUT, SETUP_REL), setup_ent)
    for rel in ["Contents/Library/LoginItems/OpalCameraStartAtLogin.app",
                "Contents/Library/USBItems/OpalCameraStartOnUsbPlugged.app",
                "Contents/Resources/OpalVirtualCamera.plugin/Contents/Resources/OpalVirtualCameraAssistant",
                "Contents/Resources/OpalVirtualCamera.plugin"]:
        src_rel = rel
        sign(os.path.join(OUT, rel), ent(src_rel))
    shim_out = "Contents/Library/LaunchServices/com.amadeus.oc.cameraExtensionShim"
    sign(os.path.join(OUT, shim_out), ent("Contents/Library/LaunchServices/com.opalcamera.cameraExtensionShim"))
    sign(OUT, ent("."))

    # verify
    run("codesign", "--verify", "--deep", "--strict", OUT)
    print("signature OK:", OUT)
    print(run("codesign", "-dv", OUT, quiet=True).stderr.strip().splitlines()[-1])


if __name__ == "__main__":
    main()
