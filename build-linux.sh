#!/bin/bash
# Mangrove build script for Linux (the Windows app is built by build.ps1, the macOS one by build-mac.sh).
# Compiles the shared core (src/core) and the GTK 4 / libadwaita interface (src/linux) with the system's C++
# compiler. Needs GTK 4.14 and libadwaita 1.5 or later (Ubuntu 24.04, Fedora 40, Debian 13 and newer), libcurl
# and libsecret, with their development files.
#
#   ./build-linux.sh             release build -> bin/mangrove
#   ./build-linux.sh --debug     debug build   -> bin/mangrove-debug
#   ./build-linux.sh --run       build and run
#   ./build-linux.sh --install   build, then install for this user (~/.local): Mangrove appears with the other apps
set -euo pipefail
root="$(cd "$(dirname "$0")" && pwd)"

config=release
run=0
install=0
for a in "$@"; do
    case "$a" in
    --debug) config=debug ;;
    --run) run=1 ;;
    --install) install=1 ;;
    *) echo "usage: $0 [--debug] [--run] [--install]" >&2; exit 2 ;;
    esac
done

cxx="${CXX:-c++}"
cc="${CC:-cc}"
command -v "$cxx" >/dev/null 2>&1 || { echo "No C++ compiler: install g++ (Ubuntu, Debian: sudo apt install build-essential)" >&2; exit 1; }
pkgs="gtk4 libadwaita-1 libcurl libsecret-1"
if ! pkg-config --exists $pkgs 2>/dev/null || ! command -v glib-compile-resources >/dev/null 2>&1; then
    echo "Missing development packages (GTK 4, libadwaita, libcurl, libsecret). To install them:" >&2
    echo "  Ubuntu, Debian:  sudo apt install build-essential pkg-config libgtk-4-dev libadwaita-1-dev libcurl4-openssl-dev libsecret-1-dev" >&2
    echo "  Fedora:          sudo dnf install gcc-c++ pkgconf-pkg-config gtk4-devel libadwaita-devel libcurl-devel libsecret-devel" >&2
    echo "  Arch:            sudo pacman -S --needed base-devel gtk4 libadwaita curl libsecret" >&2
    exit 1
fi
pkg-config --atleast-version=4.14 gtk4 && pkg-config --atleast-version=1.5 libadwaita-1 ||
    { echo "Mangrove needs GTK 4.14 and libadwaita 1.5 or later (found GTK $(pkg-config --modversion gtk4), libadwaita $(pkg-config --modversion libadwaita-1))" >&2; exit 1; }

name=mangrove
[ "$config" = debug ] && name=mangrove-debug
obj="$root/build/linux-$config"
mkdir -p "$obj" "$root/bin"

read -r -a cflags <<<"$(pkg-config --cflags $pkgs)"
read -r -a libs <<<"$(pkg-config --libs $pkgs)"
flags=(-std=c++20 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -I"$root/src/core" -I"$root/src/linux" "${cflags[@]}")
ldflags=(-pthread)
if [ "$config" = debug ]; then
    flags+=(-O0 -g -DDEBUG)
else
    flags+=(-O2 -DNDEBUG -ffunction-sections -fdata-sections)
    ldflags+=(-Wl,--gc-sections -Wl,--as-needed -s)
fi

# The styles, the icons and the shortcuts window, built into the app.
glib-compile-resources --sourcedir="$root/src/linux/data" --generate-source --target="$obj/resources.c" "$root/src/linux/data/mangrove.gresource.xml"

# Every source in parallel, a few at a time; the errors of all of them are reported.
sources=()
for f in "$root"/src/core/*.cpp; do
    [ "$(basename "$f")" = platform_win.cpp ] || sources+=("$f")
done
sources+=("$root"/src/linux/*.cpp)
jobs="$(nproc 2>/dev/null || echo 4)"
objects=()
failed=0
running=0
for src in "${sources[@]}"; do
    o="$obj/$(basename "$(dirname "$src")")-$(basename "$src").o"
    objects+=("$o")
    "$cxx" "${flags[@]}" -c "$src" -o "$o" &
    running=$((running + 1))
    if [ "$running" -ge "$jobs" ]; then
        wait -n || failed=1
        running=$((running - 1))
    fi
done
"$cc" -O2 "${cflags[@]}" -c "$obj/resources.c" -o "$obj/resources.o" || failed=1
while [ "$running" -gt 0 ]; do
    wait -n || failed=1
    running=$((running - 1))
done
[ "$failed" = 0 ] || { echo "Build failed." >&2; exit 1; }

"$cxx" "${flags[@]}" "${objects[@]}" "$obj/resources.o" "${libs[@]}" "${ldflags[@]}" -o "$root/bin/$name"
echo "Built $root/bin/$name ($(du -h "$root/bin/$name" | cut -f1))"

[ "$install" = 1 ] && bash "$root/src/linux/data/install.sh" "$root/bin/$name"
[ "$run" = 1 ] && exec "$root/bin/$name"
exit 0
