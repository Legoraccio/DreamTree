#!/bin/bash
# Builds patchtest and runs it on a throwaway repository in the temp folder: every way of staging,
# unstaging and discarding hunks and lines that the diff view offers, checked against git (see
# patchtest.cpp). The macOS and Linux counterpart of run.ps1, with the same files and cases.
#
#   ./tools/patchtest/run.sh [cases]
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
cases="${1:-150}"
out="$root/build/patchtest"
mkdir -p "$out"
sources=("$here/patchtest.cpp" "$root/src/core/git.cpp" "$root/src/core/text.cpp" "$root/src/core/platform_posix.cpp")
if [ "$(uname)" = Darwin ]; then
    xcrun clang++ -std=c++20 -O2 -Wall -Wno-unused-parameter -mmacosx-version-min=12.0 -I"$root/src/core" "${sources[@]}" \
        -framework CoreFoundation -framework CoreServices -framework Security -lcurl -o "$out/patchtest"
else
    read -r -a cflags <<<"$(pkg-config --cflags libcurl libsecret-1)"
    read -r -a libs <<<"$(pkg-config --libs libcurl libsecret-1)"
    "${CXX:-c++}" -std=c++20 -O2 -Wall -Wno-unused-parameter -I"$root/src/core" "${cflags[@]}" "${sources[@]}" "${libs[@]}" -pthread \
        -o "$out/patchtest"
fi

repo="$(mktemp -d "${TMPDIR:-/tmp}/mangrove-patchtest.XXXXXX")"
trap 'rm -rf "$repo"' EXIT
g() { git -C "$repo" "$@" >/dev/null; }

g init -q -b main
g config user.name Test
g config user.email test@example.com
g config core.autocrlf false

printf '%s' "$(for i in $(seq 1 60); do printf 'line %02d\n' "$i"; done)" >"$repo/a.txt"  # no final line break
printf 'a\nb\nc' >"$repo/b.txt"                       # no final line break
printf 'x1\r\nx2\r\nx3\r\n' >"$repo/c.txt"            # CRLF
for i in $(seq 1 8); do printf 'e%d\n' "$i"; done >"$repo/e.txt"
for k in $(seq 1 12); do for i in $(seq 1 8); do printf 'rep %d\n' "$i"; done; done >"$repo/f.txt"  # the same block 12 times
g add -A
g commit -qm base

# a.txt: the same changes as run.ps1
{
    for i in $(seq 1 60); do
        line=$(printf 'line %02d' "$i")
        case "$i" in
        5) printf 'new 5a\nnew 5b\nnew 5c\n' ;;                  # 2 lines replaced by 3 (with line 6)
        6 | 12 | 13 | 14 | 51 | 52 | 53) ;;                      # removed
        20) printf '%s\nins 20a\nins 20b\n' "$line" ;;            # pure insertion
        22) printf 'new 22\n' ;;                                  # 1 -> 1, in the same hunk as the insertion
        40) printf 'new 40a\nnew 40b\n' ;;                        # 1 -> 2
        50) printf 'new 50\n' ;;                                  # 4 -> 1 (with lines 51-53)
        60) printf 'new 60\n' ;;                                  # the last line gains its line break
        *) printf '%s\n' "$line" ;;
        esac
    done
} >"$repo/a.txt"
printf 'a\nB\nc\nd' >"$repo/b.txt"                     # no final line break on either side
printf 'x1\r\nX2\r\nx3\r\nx4\r\n' >"$repo/c.txt"
rm "$repo/e.txt"                                       # deleted file
for k in $(seq 1 12); do for i in $(seq 1 8); do
    if [ "$i" = 4 ]; then printf 'chg 4a\nchg 4b\n'; else printf 'rep %d\n' "$i"; fi
done; done >"$repo/f.txt"
for i in $(seq 1 6); do printf 'd%d\n' "$i"; done >"$repo/d.txt"  # new file, staged for the unstage tests

failed=0
for mode in stage discard; do
    for f in a.txt b.txt c.txt e.txt f.txt; do
        "$out/patchtest" "$repo" "$f" "$mode" "$cases" 7 || failed=$((failed + 1))
    done
done
g add -A
for f in a.txt b.txt c.txt d.txt e.txt f.txt; do
    "$out/patchtest" "$repo" "$f" unstage "$cases" 13 || failed=$((failed + 1))
done
[ "$failed" = 0 ] || { echo "$failed patchtest runs failed" >&2; exit 1; }
echo "patchtest: all runs passed"
