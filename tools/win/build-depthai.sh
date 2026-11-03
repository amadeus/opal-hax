#!/bin/bash
# build-depthai.sh — build public depthai-core at the commit Opal's private fork is based on, with Opal's camera
# firmware embedded instead of Luxonis' download. Output: tools/vendor/depthai-build-<platform>/
# Base commit: luxonis/depthai-core a3c01e446 (branch custom_uvc_uac_scripting, 2022-11-18): the last public commit
# with Opal's bootloader (bb9e4c86) and depthai-shared 0b456575 (adds GlobalProperties.imageManipAdjustCmxSize, which
# Opal's firmware f57b4691 requires; the older "audioproc" commit lacks it and the firmware rejects its pipelines).
# Works on macOS (to test against the camera here) and on Windows (Git Bash + MSVC/Ninja, see README).
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
TOOLS="$(cd "$HERE/.." && pwd)"
SRC="$TOOLS/vendor/depthai-core-opal"
BASE=a3c01e446cc94fd4bc6dc668da11b8cbca4de979
FW="$TOOLS/vendor/opal-fw"
case "$(uname -s)" in Darwin) PLAT=mac;; MINGW*|MSYS*|CYGWIN*) PLAT=win;; *) PLAT=linux;; esac
BUILD="${BUILD:-$TOOLS/vendor/depthai-build-$PLAT}"
if [ ! -d "$SRC" ]; then
  git clone -q https://github.com/luxonis/depthai-core.git "$SRC"
  git -C "$SRC" fetch -q origin custom_uvc_uac_scripting
  git -C "$SRC" checkout -q "$BASE"
  git -C "$SRC" submodule update -q --init shared/depthai-shared shared/depthai-bootloader-shared
fi
DEV=$(ls "$FW"/depthai-device-fwp-*.tar.xz 2>/dev/null | head -1)
BL=$(ls "$FW"/depthai-bootloader-fwp-*.tar.xz 2>/dev/null | head -1)
[ -n "$DEV" ] && [ -n "$BL" ] || { echo "no firmware in $FW: run extract-opal-fw.py (on a Mac with Opal.app)"; exit 1; }
DEV_HASH=$(basename "$DEV" | sed -E 's/.*fwp-([0-9a-f]{40}).*/\1/')
BL_HASH=$(basename "$BL" | sed -E 's/.*fwp-([0-9a-f]{40}).*/\1/')
# point the build at Opal's firmware commits
sed -i.bak -E "s/^set\(DEPTHAI_DEVICE_SIDE_COMMIT \"[0-9a-f]*\"\)/set(DEPTHAI_DEVICE_SIDE_COMMIT \"$DEV_HASH\")/" "$SRC/cmake/Depthai/DepthaiDeviceSideConfig.cmake"
sed -i.bak -E "s/^set\(DEPTHAI_BOOTLOADER_VERSION \"[0-9a-f]*\"\)/set(DEPTHAI_BOOTLOADER_VERSION \"$BL_HASH\")/" "$SRC/cmake/Depthai/DepthaiBootloaderConfig.cmake"
rm -f "$SRC"/cmake/Depthai/*.bak
# zlib 1.2.11 (pinned in depthai's Hunter config) breaks on current macOS SDKs (its fdopen macro); use 1.3.1
python3 - "$SRC/cmake/Hunter/config.cmake" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
s = s.replace('VERSION "1.2.11-p2"\n    URL "https://github.com/luxonis/zlib/archive/refs/tags/v1.2.11-p2.tar.gz"\n    SHA1 "fb8b6486183b13a86040f793a939b128f6d27095"',
              'VERSION "1.3.1-p0"\n    URL "https://github.com/cpp-pm/zlib/archive/v1.3.1-p0.tar.gz"\n    SHA1 "95a5c97dda575b1487d30760c07330e078ffc3c9"')
open(p, "w").write(s)
PY
# a3c01e446 has the payload copy in StreamMessageParser::parseMessageToADatatype commented out ("TMPTMP", unfinished
# zero-copy work): every message reaches the host with empty data. Restore it.
python3 - "$SRC/src/pipeline/datatype/StreamMessageParser.cpp" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
s = s.replace("""    // std::vector<uint8_t> data(packet->data, packet->data + bufferLength);
    std::vector<uint8_t> data;""", """    std::vector<uint8_t> data(packet->data, packet->data + bufferLength);  // opal-win: restored""")
open(p, "w").write(s)
PY
grep -q "opal-win: restored" "$SRC/src/pipeline/datatype/StreamMessageParser.cpp" || { echo "StreamMessageParser patch did not apply"; exit 1; }

# pre-seed the resources folder: the downloader skips files that already exist
mkdir -p "$BUILD/resources"
cp "$DEV" "$BL" "$BUILD/resources/"
git -C "$SRC/shared/depthai-shared" rev-parse HEAD > "$BUILD/resources/depthai-shared-commit-hash-$DEV_HASH.txt"
git -C "$SRC/shared/depthai-bootloader-shared" rev-parse HEAD > "$BUILD/resources/depthai-bootloader-shared-commit-hash-$BL_HASH.txt"
# CMake 4 dropped <3.5 compatibility; Hunter sub-builds only see the environment variable
export CMAKE_POLICY_VERSION_MINIMUM=3.5
GEN=()
command -v ninja >/dev/null && GEN=(-G Ninja)
# re-running configure on an existing Hunter build dir fails ("toolchain.cmake: Internal error"); configure once
[ -f "$BUILD/CMakeCache.txt" ] || cmake -S "$SRC" -B "$BUILD" "${GEN[@]}" -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DDEPTHAI_OPENCV_SUPPORT=OFF -DDEPTHAI_ENABLE_BACKWARD=OFF \
  -DDEPTHAI_BUILD_EXAMPLES=OFF -DDEPTHAI_BUILD_TESTS=OFF -DDEPTHAI_CLANG_FORMAT=OFF \
  -DCMAKE_INSTALL_PREFIX="$BUILD/install" "$@"
cmake --build "$BUILD" --config Release --parallel
cmake --install "$BUILD" --config Release
echo "depthai-core with Opal firmware $DEV_HASH installed in $BUILD/install"
