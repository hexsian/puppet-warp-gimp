<#
.SYNOPSIS
    Puppet Warp for GIMP - Installer (Windows, offline: copies the files next to this script)
    Repository: https://github.com/hexsian/puppet-warp-gimp

.EXAMPLE
    # Run from the extracted release folder (or just double-click install.bat)
    powershell -ExecutionPolicy Bypass -File .\install.ps1
    powershell -ExecutionPolicy Bypass -File .\install.ps1 -Action uninstall

    To update: download the newer release zip, extract it, run the installer again.
#>
[CmdletBinding()]
param(
    [ValidateSet('install', 'update', 'uninstall', 'help')]
    [string]$Action = 'install'
)

$ErrorActionPreference = 'Stop'

$PluginName = 'puppet-warp'
$GeglModule = 'puppet-warp.dll'
$SrcDir     = Split-Path -Parent $MyInvocation.MyCommand.Path
$Skip       = @('install.sh', 'install.ps1', 'install.bat', '.git', '.github', '.gitignore', '__pycache__')

function Log-Info { param($m) Write-Host "[INFO]  $m" -ForegroundColor Cyan }
function Log-Ok   { param($m) Write-Host "[OK]    $m" -ForegroundColor Green }
function Log-Warn { param($m) Write-Host "[WARN]  $m" -ForegroundColor Yellow }
function Log-Err  { param($m) Write-Host "[ERROR] $m" -ForegroundColor Red }

# %APPDATA%\GIMP\3.x\plug-ins\puppet-warp
function Get-PluginDirs {
    $root = Join-Path $env:APPDATA 'GIMP'
    $dirs = @()
    if (Test-Path $root) {
        Get-ChildItem -Path $root -Directory -Filter '3.*' | ForEach-Object {
            $dirs += (Join-Path $_.FullName "plug-ins\$PluginName")
        }
    }
    if ($dirs.Count -eq 0) {
        $dirs += (Join-Path $root "3.0\plug-ins\$PluginName")
        $dirs += (Join-Path $root "3.2\plug-ins\$PluginName")
    }
    return $dirs
}

# GEGL looks in GLib's user data dir, which on Windows is normally
# %LOCALAPPDATA%; %APPDATA% is included as a fallback.
function Get-GeglDirs {
    return @(
        (Join-Path $env:LOCALAPPDATA 'gegl-0.4\plug-ins'),
        (Join-Path $env:APPDATA 'gegl-0.4\plug-ins')
    )
}

function Install-Plugin {
    param([string]$Target)

    if (Test-Path $Target) {
        Log-Info "Replacing existing installation: $Target"
        Remove-Item -Recurse -Force $Target
    }
    else {
        Log-Info "Installing into: $Target"
    }
    New-Item -ItemType Directory -Force -Path $Target | Out-Null

    # Do NOT copy native binaries (*.dll / *.so) into the plug-in folder.
    # They are deployed only into the GEGL user plug-in directory below.
    Get-ChildItem -Path $SrcDir -Force |
        Where-Object {
            ($Skip -notcontains $_.Name) -and
            ($_.Extension -ne '.zip')   -and
            ($_.Extension -ne '.dll')   -and
            ($_.Extension -ne '.so')
        } |
        ForEach-Object { Copy-Item -Path $_.FullName -Destination $Target -Recurse -Force }
    Log-Ok 'Copied plugin files (Python sources only; native binaries go to GEGL dir).'

    $dll = Join-Path $SrcDir $GeglModule
    if (Test-Path $dll) {
        foreach ($g in Get-GeglDirs) {
            New-Item -ItemType Directory -Force -Path $g | Out-Null
            Copy-Item -Force $dll $g
            Log-Ok "Deployed $GeglModule -> $g"
        }
    }
    else {
        Log-Warn "No prebuilt $GeglModule included."
        Log-Warn 'Without it GIMP shows: the filter "custom:puppet-warp" is not installed.'
        Log-Info "Put a Windows-built $GeglModule next to this installer and run it again,"
        Log-Info 'or build it with MSYS2 (gcc + pkg-config + gegl/glib dev) and copy it to:'
        foreach ($g in Get-GeglDirs) { Log-Info "  $g" }
    }
}

function Uninstall-All {
    Log-Warn 'Starting uninstallation...'
    foreach ($d in Get-PluginDirs) {
        if (Test-Path $d) { Remove-Item -Recurse -Force $d; Log-Ok "Removed: $d" }
    }
    foreach ($g in Get-GeglDirs) {
        $f = Join-Path $g $GeglModule
        if (Test-Path $f) { Remove-Item -Force $f; Log-Ok "Removed: $f" }
    }
    Log-Ok 'Uninstallation completed.'
}

switch ($Action) {
    'help'      { Write-Host 'Usage: install.ps1 [-Action install|uninstall]' }
    'uninstall' { Uninstall-All }
    default {
        if (-not (Test-Path (Join-Path $SrcDir "$PluginName.py"))) {
            Log-Err "$PluginName.py not found next to install.ps1 ($SrcDir)."
            Log-Err 'Run this script from the extracted release folder.'
            exit 1
        }
        Log-Info 'Starting Puppet Warp GIMP plug-in deployment...'
        foreach ($d in Get-PluginDirs) { Install-Plugin -Target $d }
        Write-Host ''
        Log-Ok 'Process finished! Please restart GIMP to apply changes.'
    }
}