#!/bin/bash
# Installs Mangrove for this user, without administrator rights: ~/.local/bin/mangrove, with its entry among the
# applications and its icon. Run it from the extracted download, or let build-linux.sh --install run it.
#
#   ./install.sh [the mangrove program]     (default: the one next to this script)
#   ./install.sh --uninstall
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
bindir="$HOME/.local/bin"
share="${XDG_DATA_HOME:-$HOME/.local/share}"
desktop="$share/applications/io.github.legoraccio.Mangrove.desktop"
icon="$share/icons/hicolor/scalable/apps/io.github.legoraccio.Mangrove.svg"

refresh() {
    command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database -q "$share/applications" || true
    command -v gtk-update-icon-cache >/dev/null 2>&1 && gtk-update-icon-cache -q -t -f "$share/icons/hicolor" 2>/dev/null || true
}

if [ "${1:-}" = --uninstall ]; then
    rm -f "$bindir/mangrove" "$desktop" "$icon"
    refresh
    echo "Mangrove was removed (its settings stay in ${XDG_CONFIG_HOME:-$HOME/.config}/mangrove)."
    exit 0
fi

program="${1:-$here/mangrove}"
[ -x "$program" ] || { echo "Not found: $program" >&2; exit 1; }
# the menu entry and the icon: next to this script (the download), or in the source tree
entry="$here/io.github.legoraccio.Mangrove.desktop"
svg="$here/io.github.legoraccio.Mangrove.svg"
[ -f "$svg" ] || svg="$here/icons/scalable/apps/io.github.legoraccio.Mangrove.svg"
install -Dm755 "$program" "$bindir/mangrove"
install -Dm644 "$svg" "$icon"
mkdir -p "$(dirname "$desktop")"
sed "s|^Exec=mangrove|Exec=$bindir/mangrove|" "$entry" >"$desktop"
refresh
echo "Installed $bindir/mangrove; Mangrove is now among the applications."
