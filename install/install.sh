#!/usr/bin/env sh
# GridTV VLC plugin installer — macOS + Linux.
#
# Installs the plugin (and its runtime deps rtmidi+liblo) so VLC finds it, using
# each OS's most reliable mechanism:
#   • macOS -> ~/Library/Application Support/gridtv + VLC_PLUGIN_PATH
#              (no modifying the signed VLC.app, no App Management, no sudo)
#   • Linux -> VLC's system plugin folder (auto-scanned, seamless; needs sudo)
#              VLC_PLUGIN_PATH turned out to be unreliable on Linux VLC builds.
#
# Usage:
#   ./install.sh                       # auto-finds the plugin beside this script
#   ./install.sh path/to/plugin.so     # explicit plugin file
set -eu

# --- 1. runtime deps (rtmidi for Launchpad MIDI, liblo for monome OSC) -------
install_deps() {
    case "$(uname -s)" in
        Darwin)
            command -v brew >/dev/null && brew install rtmidi liblo >/dev/null \
                && echo "gridtv: deps ready (brew rtmidi liblo)"
            ;;
        Linux)
            if command -v apt-get >/dev/null; then
                sudo apt-get update -qq && sudo apt-get install -y librtmidi-dev liblo-dev >/dev/null \
                    && echo "gridtv: deps ready (apt librtmidi-dev liblo-dev)"
            fi
            ;;
    esac
}
install_deps || echo "gridtv: could not install deps automatically — install rtmidi + liblo if the plugin won't load."

# --- 2. locate the plugin ----------------------------------------------------
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

# --- 3. install per OS -------------------------------------------------------
OS="$(uname -s)"
case "$OS" in
    Darwin)
        DEST="$HOME/Library/Application Support/gridtv"
        mkdir -p "$DEST"
        cp -f "$PLUGIN" "$DEST/"
        # GUI-launched VLC.app picks this up for the current session:
        launchctl setenv VLC_PLUGIN_PATH "$DEST" 2>/dev/null || true
        echo "gridtv: installed -> $DEST/$(basename "$PLUGIN")"
        cat <<EOF

Done. Verify (open a NEW terminal):
    vlc --list | grep -i gridtv

Make it permanent — add to ~/.zprofile so future sessions inherit it:
    export VLC_PLUGIN_PATH="$DEST"

Then enable GridTV in VLC -> Settings (Show All) -> Video -> Filters -> GridTV.
EOF
        ;;
    Linux)
        # Find VLC's system plugin dir (try pkg-config, then common locations).
        VLP=""
        for d in "$(pkg-config --variable=plugindir vlc-plugin 2>/dev/null)" \
                 /usr/lib/vlc/plugins \
                 /usr/lib/x86_64-linux-gnu/vlc/plugins \
                 /usr/lib/aarch64-linux-gnu/vlc/plugins; do
            [ -n "$d" ] && [ -d "$d" ] && { VLP="$d"; break; }
        done
        [ -n "$VLP" ] || { echo "gridtv: can't find VLC's plugins directory." >&2; exit 1; }
        sudo mkdir -p "$VLP/video_filter"
        sudo cp -f "$PLUGIN" "$VLP/video_filter/"
        echo "gridtv: installed -> $VLP/video_filter/$(basename "$PLUGIN") (system-wide)"
        cat <<EOF

Done. Verify (VLC auto-detects the new file; if not, refresh once):
    vlc --reset-plugins-cache
    vlc --list | grep -i gridtv

Then enable GridTV in VLC -> Settings (Show All) -> Video -> Filters -> GridTV.
EOF
        ;;
    *)
        echo "gridtv: '$OS' unsupported (on Windows, use install/windows.ps1)." >&2
        exit 1
        ;;
esac
