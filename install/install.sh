#!/usr/bin/env sh
# GridTV VLC plugin installer — macOS + Linux.
#
# Sets up everything the plugin needs to load in VLC:
#   1. installs the runtime deps (rtmidi for Launchpad MIDI, liblo for monome OSC)
#   2. copies the plugin into a folder you own
#   3. points VLC at it with the VLC_PLUGIN_PATH environment variable
#
# No admin rights needed for the plugin itself, no modifying the signed VLC.app /
# VLC install, no broken code signature, survives VLC updates. (Verified: VLC
# scans VLC_PLUGIN_PATH and loads libgridtv_plugin from there, cold cache.)
#
# Usage:
#   ./install.sh                       # auto-finds the plugin beside this script
#   ./install.sh path/to/plugin.so     # explicit plugin file
set -eu

# --- 1. runtime deps (best-effort; non-fatal if it can't) --------------------
install_deps() {
    case "$(uname -s)" in
        Darwin)
            command -v brew >/dev/null || { echo "gridtv: Homebrew not found — install rtmidi + liblo yourself." >&2; return 0; }
            brew install rtmidi liblo >/dev/null && echo "gridtv: deps ready (brew rtmidi liblo)"
            ;;
        Linux)
            if command -v apt-get >/dev/null; then
                sudo apt-get update -qq && sudo apt-get install -y librtmidi-dev liblo-dev >/dev/null \
                    && echo "gridtv: deps ready (apt librtmidi-dev liblo-dev)"
            else
                echo "gridtv: install rtmidi + liblo via your distro's package manager." >&2
            fi
            ;;
    esac
}
install_deps || echo "gridtv: could not install deps automatically — install rtmidi + liblo if the plugin won't load."

# --- 2. locate + copy the plugin --------------------------------------------
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
