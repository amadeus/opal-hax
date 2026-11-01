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
 5. build our code in: opalcore.dylib -> camera-driver XPC service, opalspy.dylib -> main app (LC_LOAD_DYLIB)
 6. entitlements per component = originals with the same replacements, minus team-restricted keys
    (application-identifier, team-identifier, associated-domains). The Setup app (system-extension host) keeps
    system-extension.install only when --profile is given (it needs a provisioning profile for that).
 7. sign inside-out with the Apple Development identity (hardened runtime), then verify.
"""
import argparse
import os
import plistlib
import shutil
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
TOOLS = os.path.join(ROOT, "tools")
SRC = os.path.join(ROOT, "Opal Backup.app")
OUT = os.path.join(ROOT, "build", "Opal.app")
IDENTITY = "Apple Development: Amadeus Demarzi (J972UZ26TC)"
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


def sign(path, entitlements=None, ident=IDENTITY):
    cmd = ["codesign", "--force", "--sign", ident, "--options", "runtime", "--timestamp=none"]
    if entitlements:
        ep = path.rstrip("/") + ".entitlements.plist"
        ep = os.path.join(ROOT, "build", os.path.basename(ep).replace(" ", "_"))
        with open(ep, "wb") as f:
            plistlib.dump(entitlements, f)
        cmd += ["--entitlements", ep]
    run(*cmd, path, quiet=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--profile", help="provisioning profile for com.amadeus.oc.OpalExtensionInstaller (System Extension)")
    args = ap.parse_args()

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
    print("built in opalcore (camera-driver service) and opalspy (main app)")

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
    for x in ["OpalCameraVideoService", "OpalCameraUpdateXPC", "OpalCameraDeviceService", "OpalCameraUninstallXPC"]:
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
