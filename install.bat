@echo off
rem Puppet Warp for GIMP - Windows launcher for install.ps1
rem Double-click to install/update, or: install.bat -Action uninstall
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" %*
echo.
pause
