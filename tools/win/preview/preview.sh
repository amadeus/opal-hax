#!/bin/bash
# preview.sh [outdir] [regular.ttf bold.ttf emFactor scale] - screenshot the Windows popover on this Mac (see preview.mm)
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
IMGUI="$HERE/../../vendor/opal-win-mingw/_deps/imgui-src"
[ -f "$IMGUI/imgui.cpp" ] || { echo "ImGui source missing: run tools/win/build-windows-on-mac.sh once"; exit 1; }
FT="$(brew --prefix freetype)"
OUT="${1:-$HERE/out}"
B="${TMPDIR:-/tmp}/opal-ui-preview"
mkdir -p "$B"
DEFS="-DIMGUI_ENABLE_FREETYPE"
INC="-I$IMGUI -I$IMGUI/backends -I$IMGUI/misc/freetype -I$FT/include/freetype2 -I$HERE/../app"
objs=()
for f in "$IMGUI"/imgui.cpp "$IMGUI"/imgui_draw.cpp "$IMGUI"/imgui_tables.cpp "$IMGUI"/imgui_widgets.cpp "$IMGUI"/misc/freetype/imgui_freetype.cpp; do
  o="$B/$(basename "$f").o"; [ "$o" -nt "$f" ] || clang++ -std=c++17 -O1 $DEFS $INC -c "$f" -o "$o"; objs+=("$o")
done
o="$B/imgui_impl_metal.o"; [ "$o" -nt "$IMGUI/backends/imgui_impl_metal.mm" ] || clang++ -std=c++17 -fobjc-arc -O1 $DEFS $INC -c "$IMGUI/backends/imgui_impl_metal.mm" -o "$o"; objs+=("$o")
clang++ -std=c++17 -O1 -fobjc-arc $DEFS $INC "$HERE/preview.mm" "$HERE/../app/ui.cpp" "$HERE/../app/fluent.cpp" "${objs[@]}" \
  -L"$FT/lib" -lfreetype -framework Metal -framework Foundation -framework ImageIO -framework CoreGraphics -framework QuartzCore -o "$B/preview"
shift || true
"$B/preview" "$OUT" "$@"
