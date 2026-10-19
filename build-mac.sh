#!/bin/bash
# DreamTree build script for macOS (the Windows app is built by build.ps1).
# Compiles the shared core (src/core) and the AppKit interface (src/macos) with the Xcode command line tools
# into an app bundle for Apple silicon and Intel Macs (macOS 12 or later), signed ad hoc.
#
#   ./build-mac.sh            release build -> bin/DreamTree.app
#   ./build-mac.sh --debug    debug build   -> bin/DreamTree-debug.app
#   ./build-mac.sh --native   only for this Mac's processor (faster)
#   ./build-mac.sh --run      build and open
set -euo pipefail
root="$(cd "$(dirname "$0")" && pwd)"

config=release
run=0
archs="arm64 x86_64"
for a in "$@"; do
    case "$a" in
    --debug) config=debug ;;
    --native) archs="$(uname -m)" ;;
    --run) run=1 ;;
    *) echo "usage: $0 [--debug] [--native] [--run]" >&2; exit 2 ;;
    esac
done

command -v xcrun >/dev/null 2>&1 || { echo "The Xcode command line tools are missing: run xcode-select --install" >&2; exit 1; }
cxx="$(xcrun --find clang++)"
sdk="$(xcrun --sdk macosx --show-sdk-path)"

name=DreamTree
[ "$config" = debug ] && name=DreamTree-debug
obj="$root/build/mac-$config"
app="$root/bin/$name.app"
mkdir -p "$obj" "$root/bin"

flags=(-isysroot "$sdk" -mmacosx-version-min=12.0 -std=c++20 -fobjc-arc -fno-objc-arc-exceptions
       -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -ferror-limit=0
       -I"$root/src/core" -I"$root/src/macos")
for a in $archs; do flags+=(-arch "$a"); done
if [ "$config" = debug ]; then flags+=(-O0 -g -DDEBUG); else flags+=(-O2 -DNDEBUG); fi

sources=()
for f in "$root"/src/core/*.cpp; do
    [ "$(basename "$f")" = platform_win.cpp ] || sources+=("$f")
done
sources+=("$root"/src/macos/*.mm)

# Every source in parallel; the errors of all of them are reported.
objects=()
pids=()
for src in "${sources[@]}"; do
    o="$obj/$(basename "$src").o"
    objects+=("$o")
    "$cxx" "${flags[@]}" -c "$src" -o "$o" &
    pids+=($!)
done
failed=0
for p in "${pids[@]}"; do wait "$p" || failed=1; done
[ "$failed" = 0 ] || { echo "Build failed." >&2; exit 1; }

rm -rf "$app"
mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources"
"$cxx" "${flags[@]}" "${objects[@]}" -framework Cocoa -framework CoreServices -framework CoreFoundation \
    -o "$app/Contents/MacOS/DreamTree"
cp "$root/src/macos/Info.plist" "$app/Contents/Info.plist"
printf 'APPL????' >"$app/Contents/PkgInfo"

# The icon: every size the system asks for, made from the 1024-pixel image.
icon="$root/src/macos/AppIcon.png"
if [ -f "$icon" ]; then
    set="$obj/AppIcon.iconset"
    rm -rf "$set"
    mkdir -p "$set"
    for s in 16 32 128 256 512; do
        sips -z "$s" "$s" "$icon" --out "$set/icon_${s}x${s}.png" >/dev/null
        sips -z $((s * 2)) $((s * 2)) "$icon" --out "$set/icon_${s}x${s}@2x.png" >/dev/null
    done
    iconutil -c icns "$set" -o "$app/Contents/Resources/AppIcon.icns"
fi

# Ad hoc signature: enough to run on this Mac. To give the app to others, sign it with a Developer ID
# certificate and notarize it.
codesign --force --sign - "$app"

echo "Built $app ($(du -sh "$app" | cut -f1))"
[ "$run" = 1 ] && open "$app"
exit 0
