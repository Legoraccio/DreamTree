#!/bin/bash
# Smoke test of the macOS interface: opens a throwaway repository (branches, a merge, tags, a remote, a stash
# and uncommitted changes) in bin/DreamTree.app, which saves pictures of its window at a few steps
# (DREAMTREE_SNAPSHOT) and quits. Fails when the app crashes, hangs or doesn't save them.
#
#   ./tools/uitest/mac-snapshots.sh [output folder]     (default: build/snapshots)
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
out="${1:-$root/build/snapshots}"
app="$root/bin/DreamTree.app/Contents/MacOS/DreamTree"
[ -x "$app" ] || { echo "Build the app first: ./build-mac.sh" >&2; exit 1; }
rm -rf "$out"
mkdir -p "$out"
work="$(mktemp -d "${TMPDIR:-/tmp}/dreamtree-ui.XXXXXX")"
trap 'rm -rf "$work"' EXIT

# ---- the repository
repo="$work/sample-app"
git init -q -b main "$repo"
g() { git -C "$repo" -c user.name="Ada Lovelace" -c user.email=ada@example.com "$@" >/dev/null; }
put() { mkdir -p "$(dirname "$repo/$1")"; printf '%b' "$2" >"$repo/$1"; }
put README.md '# Sample app\n\nA small project used to test DreamTree.\n'
put src/main.c '#include <stdio.h>\n\nint main(void) {\n    printf("hello\\n");\n    return 0;\n}\n'
g add -A
g commit -qm "Initial commit"
put src/util.c 'int add(int a, int b) { return a + b; }\n'
g add -A
g commit -qm "Add the util module" -m "Small helpers shared by the commands."
g switch -q -c feature/login
put src/login.c 'int login(const char* user) {\n    return user != 0;\n}\n'
g add -A
g commit -qm "Add a login screen"
put src/login.c 'int login(const char* user) {\n    if (!user) return 0;\n    return 1;\n}\n'
g commit -qam "Check the user name before logging in"
g switch -q main
put README.md '# Sample app\n\nA small project used to test DreamTree.\n\n## Building\n\nRun make.\n'
g commit -qam "Explain how to build"
g merge -q --no-ff feature/login -m "Merge branch 'feature/login'"
g tag v1.0
g switch -q -c fix/typo HEAD~1
put README.md '# Sample app\n\nA small project used to try DreamTree.\n\n## Building\n\nRun make.\n'
g commit -qam "Fix a typo in the readme"
g switch -q main
git init -q --bare "$work/origin.git"
g remote add origin "$work/origin.git"
g push -q -u origin main feature/login
put src/util.c 'int add(int a, int b) { return a + b; }\nint sub(int a, int b) { return a - b; }\n'
g commit -qam "Add sub()"
put src/main.c '#include <stdio.h>\n\nint main(void) {\n    printf("hello, stash\\n");\n    return 0;\n}\n'
g stash push -q -m "try another greeting"
# uncommitted changes: a modified file, part of it staged, and a new file
put src/main.c '#include <stdio.h>\n#include "util.h"\n\nint main(void) {\n    printf("%d\\n", add(2, 3));\n    return 0;\n}\n'
put src/util.c 'int add(int a, int b) { return a + b; }\nint sub(int a, int b) { return a - b; }\nint mul(int a, int b) { return a * b; }\n'
g add src/util.c
put src/util.h 'int add(int a, int b);\nint sub(int a, int b);\nint mul(int a, int b);\n'

# ---- the app
settings="$work/settings.ini"
printf 'tab=%s\nactive=0\nrecent=%s\naccount=github\thttps://github.com\toctocat\tThe Octocat\n' "$repo" "$repo" >"$settings"
DREAMTREE_SETTINGS="$settings" DREAMTREE_SNAPSHOT="$out" "$app" >"$out/app.log" 2>&1 &
pid=$!
status=0
# A real screenshot whenever the app says a step is on screen ("<step>.ready"): the system draws the glass of
# the sidebar and of the inspector itself, and the app's own pictures can't show it.
for _ in $(seq 1 360); do
    kill -0 "$pid" 2>/dev/null || break
    for ready in "$out"/*.ready; do
        [ -e "$ready" ] || continue
        shot="${ready%.ready}-screen.png"
        [ -e "$shot" ] && continue
        screencapture -x "$shot" &
        cap=$!
        for _ in 1 2 3 4 5 6 7 8 9 10; do kill -0 "$cap" 2>/dev/null || break; sleep 0.5; done
        kill "$cap" 2>/dev/null || true
        touch "$shot"
    done
    sleep 0.5
done
if kill -0 "$pid" 2>/dev/null; then
    echo "The app did not finish in 3 minutes." >&2
    kill "$pid" || true
    status=1
else
    wait "$pid" || status=$?
fi
cat "$out/app.log"
ls -l "$out"
for f in 1-changes 2-commit 3-diff 4-diff-split 5-settings-general 6-settings-appearance 7-settings-git 8-settings-integrations 9-add-account; do
    [ -s "$out/$f.png" ] || { echo "Missing $f.png" >&2; status=1; }
done
exit "$status"
