#!/usr/bin/env bash
#
# install-plugin.sh — install/upgrade the GridTV VLC plugin into VLC.app.
#
# Run from your own Terminal (NOT through a sandboxed agent):
#   ~/gridtv/scripts/install-plugin.sh
#
# Why this is scripted with AppleScript/Finder: macOS "App Management" blocks
# even `sudo cp` from writing into /Applications/VLC.app (you'd get
# "Operation not permitted"). Finder already has that permission, so we drive
# the copy through Finder. You'll get ONE system password/auth prompt.
#
# If Finder is also blocked on your machine, either:
#   - System Settings → Privacy & Security → App Management → add Terminal, or
#   - do the manual drag described in README.md.

set -euo pipefail

# This installer is macOS-only (it drives Finder to bypass App Management).
# Linux/Windows: copy the built plugin into VLC's plugins dir manually —
# see README.md "Setup by operating system".
if [ "$(uname -s)" != "Darwin" ]; then
    echo "✗ install-plugin.sh is macOS-only. On $(uname -s), copy the built plugin"
    echo "  (libgridtv_plugin.so / gridtv_plugin.dll) into VLC's plugins folder"
    echo "  and run 'vlc --reset-plugins-cache'. See README.md for the exact path."
    exit 1
fi

cd "$(dirname "$0")/.."

VLC_APP="${VLC_APP:-/Applications/VLC.app}"
PLUGINS="$VLC_APP/Contents/MacOS/plugins"
SRC="$PWD/build/libgridtv_plugin.dylib"

[ -d "$PLUGINS" ] || { echo "✗ VLC not found: $PLUGINS"; exit 1; }

echo "==> Building the plugin…"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release >/dev/null 2>&1 || true
cmake --build build --target gridtv_plugin -j >/dev/null
[ -f "$SRC" ] || { echo "✗ Build failed — no plugin at $SRC"; exit 1; }

echo "==> Installing into VLC.app via Finder (authenticate when prompted)…"
# Delete the existing plugin + stale cache, then copy the new one. All done by
# Finder so App Management allows it.
osascript <<APPLESCRIPT
set srcAlias to (POSIX file "$SRC") as alias
set destAlias to (POSIX file "$PLUGINS") as alias
tell application "Finder"
    activate
    if exists file "libgridtv_plugin.dylib" of destAlias then
        delete file "libgridtv_plugin.dylib" of destAlias
    end if
    if exists file "plugins.dat" of destAlias then
        delete file "plugins.dat" of destAlias
    end if
    duplicate srcAlias to destAlias
end tell
APPLESCRIPT

# Tidy up: remove a stray copy if it was ever dropped in the wrong folder.
if [ -f "$VLC_APP/Contents/MacOS/lib/libgridtv_plugin.dylib" ]; then
    osascript -e 'set f to (POSIX file "'"$VLC_APP"'/Contents/MacOS/lib/libgridtv_plugin.dylib") as alias' \
              -e 'tell application "Finder" to delete f' 2>/dev/null || true
fi

echo "==> Verifying…"
if [ -f "$PLUGINS/libgridtv_plugin.dylib" ]; then
    echo "✓ Installed: $PLUGINS/libgridtv_plugin.dylib"
    echo "  VLC detects:"
    "$VLC_APP/Contents/MacOS/VLC" --list 2>/dev/null | grep -i gridtv || echo "  (restart VLC if it's running)"
    echo
    echo "Enable it in: VLC → Preferences (Show All) → Video → Filters → GridTV"
    echo "  (or launch with: scripts/gridtv-vlc video.mp4)"
else
    echo "✗ Install didn't complete. Try the manual Finder drag (see README.md)."
    exit 1
fi
