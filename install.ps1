<#
.SYNOPSIS
    Puppet Warp for GIMP - Installer & Updater (Windows)
    Repository: https://github.com/hexsian/puppet-warp-gimp

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\install.ps1              # install / update
    powershell -ExecutionPolicy Bypass -File .\install.ps1 -Action update
    powershell -ExecutionPolicy Bypass -File .\install.ps1 -Action uninstall
#>
[CmdletBinding()]
param(
    [ValidateSet('install', 'update', 'uninstall', 'help')]
    [string]$Action = 'install'
)

$ErrorActionPreference = 'Stop'

$RepoUrl    = 'https://github.com/hexsian/puppet-warp-gimp.git'
$ZipUrl     = 'https://github.com/hexsian/puppet-warp-gimp/archive/HEAD.zip'
$PluginName = 'puppet-warp'
$GeglModule = 'puppet-warp.dll'

function Log-Info    { param($m) Write-Host "[INFO]  $m" -ForegroundColor Cyan }
function Log-Ok      { param($m) Write-Host "[OK]    $m" -ForegroundColor Green }
function Log-Warn    { param($m) Write-Host "[WARN]  $m" -ForegroundColor Yellow }
function Log-Err     { param($m) Write-Host "[ERROR] $m" -ForegroundColor Red }

# GIMP 3 plug-in folders: %APPDATA%\GIMP\3.x\plug-ins\puppet-warp
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

# GEGL user module folder. GLib's user data dir on Windows is %APPDATA%,
# which is where the plug-in itself looks (GLib.get_user_data_dir()).
function Get-GeglDirs {
    return @(Join-Path $env:APPDATA 'gegl-0.4\plug-ins')
}

function Has-Git { return [bool](Get-Command git -ErrorAction SilentlyContinue) }

function Install-ViaZip {
    param([string]$Target)
    $tmp = Join-Path ([IO.Path]::GetTempPath()) ("pw-" + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $tmp | Out-Null
    try {
        $zip = Join-Path $tmp 'repo.zip'
        Log-Info 'git not found, downloading ZIP instead...'
        [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
        Invoke-WebRequest -Uri $ZipUrl -OutFile $zip -UseBasicParsing
        Expand-Archive -Path $zip -DestinationPath $tmp -Force
        $src = Get-ChildItem -Path $tmp -Directory | Select-Object -First 1
        if (-not $src) { throw 'Downloaded archive is empty.' }
        if (Test-Path $Target) { Remove-Item -Recurse -Force $Target }
        Move-Item -Path $src.FullName -Destination $Target
    }
    finally {
        Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
    }
}

function Install-Or-Update {
    param([string]$Target)

    New-Item -ItemType Directory -Force -Path (Split-Path $Target -Parent) | Out-Null

    if (Has-Git) {
        if (Test-Path (Join-Path $Target '.git')) {
            Log-Info "Existing Git installation detected: $Target"
            git -C $Target fetch --quiet origin
            git -C $Target pull --ff-only --quiet
            if ($LASTEXITCODE -ne 0) {
                git -C $Target reset --hard origin/HEAD --quiet
            }
            Log-Ok "Updated $PluginName to latest commit."
        }
        else {
            if (Test-Path $Target) {
                Log-Warn "Directory exists without Git metadata, reinstalling: $Target"
                Remove-Item -Recurse -Force $Target
            }
            Log-Info "Installing into: $Target"
            git clone --quiet $RepoUrl $Target
            if ($LASTEXITCODE -ne 0) { throw 'git clone failed.' }
            Log-Ok 'Installed successfully.'
        }
    }
    else {
        Install-ViaZip -Target $Target
        Log-Ok 'Installed from ZIP (re-run this script to update).'
    }

    if (-not (Test-Path (Join-Path $Target "$PluginName.py"))) {
        Log-Warn "Entry point $PluginName.py not found in $Target"
    }

    # Prebuilt GEGL module (Windows .dll), if the repo ships one.
    $dll = Join-Path $Target $GeglModule
    if (Test-Path $dll) {
        foreach ($g in Get-GeglDirs) {
            New-Item -ItemType Directory -Force -Path $g | Out-Null
            Copy-Item -Force $dll $g
            Log-Ok "Deployed $GeglModule -> $g"
        }
    }
    else {
        Log-Warn "No prebuilt $GeglModule in the repository."
        Log-Info 'The plug-in will try to compile it on first run with gcc + pkg-config'
        Log-Info '(MSYS2 MinGW-w64 with gegl and glib2 dev packages, available in PATH).'
    }
}

function Uninstall-All {
    Log-Warn 'Starting uninstallation...'
    foreach ($d in Get-PluginDirs) {
        if (Test-Path $d) {
            Remove-Item -Recurse -Force $d
            Log-Ok "Removed: $d"
        }
    }
    foreach ($g in Get-GeglDirs) {
        $f = Join-Path $g $GeglModule
        if (Test-Path $f) {
            Remove-Item -Force $f
            Log-Ok "Removed: $f"
        }
    }
    Log-Ok 'Uninstallation completed.'
}

switch ($Action) {
    'help' {
        Write-Host 'Usage: install.ps1 [-Action install|update|uninstall]'
    }
    'uninstall' {
        Uninstall-All
    }
    default {
        Log-Info 'Starting Puppet Warp GIMP plug-in deployment...'
        foreach ($d in Get-PluginDirs) {
            Install-Or-Update -Target $d
        }
        Write-Host ''
        Log-Ok 'Process finished! Please restart GIMP to apply changes.'
        Log-Info 'To update in the future, simply run this script again.'
    }
}
