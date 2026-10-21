#!/bin/bash
# Single instance on Linux: a second start of bin/mangrove with the same settings hands the folder on its command
# line to the first one, which opens it in a tab, and quits at once. Without a display or a session bus, it runs
# on virtual ones.
#
#   ./tools/uitest/linux-single-instance.sh
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
app="$root/bin/mangrove"
[ -x "$app" ] || { echo "Build the app first: ./build-linux.sh" >&2; exit 1; }
if [ -z "${MANGROVE_TEST_INNER:-}" ]; then
    export MANGROVE_TEST_INNER=1
    run=("$0")
    [ -z "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ] && run=(xvfb-run -a -s "-screen 0 1400x900x24" "${run[@]}")
    [ -z "${DBUS_SESSION_BUS_ADDRESS:-}" ] && run=(dbus-run-session -- "${run[@]}")
    exec "${run[@]}"
fi

work="$(mktemp -d "${TMPDIR:-/tmp}/mangrove-instance.XXXXXX")"
p1=""
trap '[ -n "$p1" ] && kill "$p1" 2>/dev/null; rm -rf "$work"' EXIT
new_repo() {
    git init -q -b main "$work/$1"
    printf '# %s\n' "$1" >"$work/$1/README.md"
    git -C "$work/$1" add -A
    git -C "$work/$1" -c user.name=Test -c user.email=test@example.com commit -qm "First commit"
}
new_repo first
new_repo second
settings="$work/settings.ini"
printf 'tab=%s\nactive=0\n' "$work/first" >"$settings"
export MANGROVE_SETTINGS="$settings" GTK_A11Y=none  # its own instance: a Mangrove already open stays out of it
has() { tr -d '\r' <"$settings" | grep -q "$1"; }  # the settings file has Windows line ends

status=0
"$app" >"$work/first.log" 2>&1 &
p1=$!
# The first instance is ready once it has opened its repository: it saves its settings then.
for _ in $(seq 1 75); do
    has "^recent=" && break
    sleep 0.2
done
has "^recent=" || { echo "The first instance did not open its repository." >&2; status=1; }

# A second start with a folder: it hands the folder over and quits.
start=$(date +%s)
if timeout 15 "$app" "$work/second" >"$work/second.log" 2>&1; then
    echo "The second start quit after $(($(date +%s) - start)) s."
else
    echo "The second start failed or did not quit (exit code $?)." >&2
    status=1
fi
kill -0 "$p1" 2>/dev/null || { echo "The first instance quit." >&2; status=1; }
for _ in $(seq 1 50); do
    has "^tab=$work/second\$" && break
    sleep 0.2
done
if has "^tab=$work/second\$"; then
    echo "The second start handed its folder to the first instance."
else
    echo "The folder of the second start didn't open in the first instance. Settings:" >&2
    cat "$settings" >&2
    status=1
fi
cat "$work/first.log" "$work/second.log"
exit "$status"
