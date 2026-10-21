#!/bin/bash
# Builds coretest with the shared core and runs it (see coretest.cpp). On macOS it needs the Xcode command line
# tools; on Linux a C++ compiler and the development files of libcurl and libsecret (see build-linux.sh).
#
#   ./tools/coretest/run.sh
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
out="$root/build/coretest"
mkdir -p "$out"

sources=("$here/coretest.cpp")
for f in "$root"/src/core/*.cpp; do
    [ "$(basename "$f")" = platform_win.cpp ] || sources+=("$f")
done
if [ "$(uname)" = Darwin ]; then
    xcrun clang++ -std=c++20 -O1 -g -Wall -Wextra -Wno-unused-parameter -mmacosx-version-min=12.0 -I"$root/src/core" \
        "${sources[@]}" -framework CoreFoundation -framework CoreServices -framework Security -lcurl -o "$out/coretest"
else
    read -r -a cflags <<<"$(pkg-config --cflags libcurl libsecret-1)"
    read -r -a libs <<<"$(pkg-config --libs libcurl libsecret-1)"
    "${CXX:-c++}" -std=c++20 -O1 -g -Wall -Wextra -Wno-unused-parameter -I"$root/src/core" "${cflags[@]}" \
        "${sources[@]}" "${libs[@]}" -pthread -o "$out/coretest"
fi
"$out/coretest"
