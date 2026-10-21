#!/bin/bash
# Smoke test of the Linux interface: opens a throwaway repository (branches, a merge, tags, a remote, a stash
# and uncommitted changes) in bin/mangrove, which saves pictures of its window at a few steps
# (MANGROVE_SNAPSHOT) and quits. Fails when the app crashes, hangs or doesn't save them. Without a display, it
# runs on a virtual one (xvfb-run).
#
#   ./tools/uitest/linux-snapshots.sh [output folder]     (default: build/snapshots)
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
out="${1:-$root/build/snapshots}"
app="$root/bin/mangrove"
[ -x "$app" ] || { echo "Build the app first: ./build-linux.sh" >&2; exit 1; }
rm -rf "$out"
mkdir -p "$out"
out="$(cd "$out" && pwd)"
work="$(mktemp -d "${TMPDIR:-/tmp}/mangrove-ui.XXXXXX")"
trap 'rm -rf "$work"' EXIT

# ---- the repository (the same as the macOS test's)
repo="$work/sample-app"
git init -q -b main "$repo"
g() { git -C "$repo" -c user.name="Ada Lovelace" -c user.email=ada@example.com "$@" >/dev/null; }
put() { mkdir -p "$(dirname "$repo/$1")"; printf '%b' "$2" >"$repo/$1"; }
put README.md '# Sample app\n\nA small project used to test Mangrove.\n'
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
put README.md '# Sample app\n\nA small project used to test Mangrove.\n\n## Building\n\nRun make.\n'
g commit -qam "Explain how to build"
g merge -q --no-ff feature/login -m "Merge branch 'feature/login'"
g tag v1.0
g switch -q -c fix/typo HEAD~1
put README.md '# Sample app\n\nA small project used to try Mangrove.\n\n## Building\n\nRun make.\n'
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
# Dark style: the pictures of the README are taken from this run.
printf 'tab=%s\nactive=0\nrecent=%s\ntheme=dark\naccount=github\thttps://github.com\toctocat\tThe Octocat\n' "$repo" "$repo" >"$settings"
# GTK set up as on a GNOME desktop (its interface font), apart from the user's own settings
export XDG_CONFIG_HOME="$work/config"
mkdir -p "$XDG_CONFIG_HOME/gtk-4.0"
printf '[Settings]\ngtk-font-name=Cantarell 11\n' >"$XDG_CONFIG_HOME/gtk-4.0/settings.ini"
run=(env MANGROVE_SETTINGS="$settings" MANGROVE_SNAPSHOT="$out" GTK_A11Y=none "$app")
if [ -z "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ]; then
    command -v xvfb-run >/dev/null 2>&1 || { echo "No display, and no xvfb-run to make one (sudo apt install xvfb)" >&2; exit 1; }
    run=(xvfb-run -a -s "-screen 0 1600x1000x24" "${run[@]}")
fi
# a session bus, as on a desktop, when there is none
if [ -z "${DBUS_SESSION_BUS_ADDRESS:-}" ] && command -v dbus-run-session >/dev/null 2>&1; then run=(dbus-run-session -- "${run[@]}"); fi
status=0
timeout 180 "${run[@]}" >"$out/app.log" 2>&1 || status=$?
[ "$status" = 124 ] && echo "The app did not finish in 3 minutes." >&2
cat "$out/app.log"
ls -l "$out"
for f in 1-changes 2-commit 3-diff 4-diff-split 5-settings-general 6-settings-appearance 7-settings-git 8-settings-integrations 9-add-account 10-sign-in; do
    [ -s "$out/$f.png" ] || { echo "Missing $f.png" >&2; status=1; }
done
grep -q "CRITICAL\|Gtk-WARNING\|Adwaita-WARNING" "$out/app.log" && { echo "The app reported problems (see app.log)." >&2; status=1; }
exit "$status"
