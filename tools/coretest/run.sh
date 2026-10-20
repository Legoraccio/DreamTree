#!/bin/bash
# Builds coretest with the shared core and runs it (see coretest.cpp). Needs the Xcode command line tools.
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
xcrun clang++ -std=c++20 -O1 -g -Wall -Wextra -Wno-unused-parameter -mmacosx-version-min=12.0 -I"$root/src/core" \
    "${sources[@]}" -framework CoreFoundation -framework CoreServices -framework Security -lcurl -o "$out/coretest"
"$out/coretest"
