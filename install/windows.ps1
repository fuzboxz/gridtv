# GridTV VLC plugin installer — Windows.
#
# Copies the downloaded plugin into %LOCALAPPDATA%\gridtv and sets the
# VLC_PLUGIN_PATH user environment variable. No admin rights, no modifying
# VLC's Program Files install. (Verified on macOS/Linux that VLC loads the
# plugin purely from VLC_PLUGIN_PATH; the same mechanism applies on Windows.)
#
# Usage (in PowerShell):
#   powershell -ExecutionPolicy Bypass -File install\windows.ps1
#   powershell -ExecutionPolicy Bypass -File install\windows.ps1 -Plugin C:\path\to\gridtv_plugin.dll
param(
    [string]$Plugin = ""
)

$ErrorActionPreference = "Stop"
$Dest = Join-Path $env:LOCALAPPDATA "gridtv"

# Locate the plugin: explicit -Plugin, else beside this script, else cwd.
if (-not $Plugin) {
    $here = Split-Path -Parent $MyInvocation.MyCommand.Path
    foreach ($c in @((Join-Path $here "libgridtv_plugin.dll"), (Join-Path $PWD "libgridtv_plugin.dll"))) {
        if (Test-Path $c) { $Plugin = $c; break }
    }
}
if (-not $Plugin -or -not (Test-Path $Plugin)) {
    Write-Error "gridtv: plugin not found. Pass -Plugin C:\path\to\gridtv_plugin.dll"
    exit 1
}

New-Item -ItemType Directory -Force -Path $Dest | Out-Null
Copy-Item -Force $Plugin $Dest
$installed = Join-Path $Dest (Split-Path -Leaf $Plugin)
Write-Host "gridtv: installed -> $installed"

# Persistent user env var (new processes / next login pick it up).
setx VLC_PLUGIN_PATH "$Dest" | Out-Null
Write-Host "gridtv: set user env VLC_PLUGIN_PATH=$Dest"

Write-Host ""
Write-Host "Done. Open a NEW terminal, then verify:"
Write-Host "    vlc --list | findstr /i gridtv"
Write-Host "Then enable GridTV in VLC -> Settings (Show All) -> Video -> Filters -> GridTV."
