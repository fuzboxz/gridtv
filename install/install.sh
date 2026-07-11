#!/usr/bin/env sh
# GridTV VLC plugin installer — macOS + Linux.
#
# Copies the downloaded plugin into a folder you own and points VLC at it with
# the VLC_PLUGIN_PATH environment variable. This needs no admin rights, does NOT
# modify the signed VLC.app / VLC install, does NOT break VLC's code signature,
# and survives VLC updates. (Verified: VLC scans VLC_PLUGIN_PATH and loads
# libgridtv_plugin from there, even with a cold plugin cache.)
#
# Usage:
#   ./install.sh                       # auto-finds the plugin beside this script
#   ./install.sh path/to/plugin.so     # explicit plugin file
set -eu

find_plugin() {
    for c in "$1" \
             "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/libgridtv_plugin.dylib" \
             "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/libgridtv_plugin.so" \
             "$PWD/libgridtv_plugin.dylib" \
             "$PWD/libgridtv_plugin.so"; do
        [ -n "${c:-}" ] && [ -f "$c" ] && { echo "$c"; return 0; }
    done
    return 1
}

PLUGIN="$(find_plugin "${1:-}")" || {
    echo "gridtv: plugin not found. Pass it explicitly:" >&2
    echo "  $0 /path/to/libgridtv_plugin.{dylib,so}" >&2
    exit 1
}

OS="$(uname -s)"
case "$OS" in
    Darwin) DEST="$HOME/Library/Application Support/gridtv" ;;
    Linux)  DEST="$HOME/.local/share/gridtv" ;;
    *) echo "gridtv: '$OS' unsupported (on Windows, use install/windows.ps1)." >&2; exit 1 ;;
esac

mkdir -p "$DEST"
cp -f "$PLUGIN" "$DEST/"
echo "gridtv: installed -> $DEST/$(basename "$PLUGIN")"
echo "gridtv: VLC_PLUGIN_PATH=$DEST"

# Make a GUI-launched VLC.app pick it up for the current session on macOS.
# (Not persistent across reboots — see the profile note below.)
if [ "$OS" = "Darwin" ]; then
    launchctl setenv VLC_PLUGIN_PATH "$DEST" 2>/dev/null || true
fi

cat <<EOF

Done. Verify (open a NEW terminal first):
    vlc --list | grep -i gridtv

To make it permanent, add this line to your shell profile (~/.zprofile on
macOS, ~/.bashrc on Linux) so future sessions inherit it:
    export VLC_PLUGIN_PATH="$DEST"

Then enable GridTV in VLC -> Settings (Show All) -> Video -> Filters -> GridTV.

Linux note: if you launch VLC from the desktop the env var may not reach it; in
that case copy straight into VLC's system folder instead:
    sudo cp "$DEST"/libgridtv_plugin.so "\$(pkg-config --variable=plugindir vlc 2>/dev/null || echo /usr/lib/vlc/plugins)/video_filter/"
    vlc --reset-plugins-cache
EOF
