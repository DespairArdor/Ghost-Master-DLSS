@echo off
rem Same launch as Start-DLSS.cmd, but the addon only measures FPS: no tracking, no DLSS.
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\launch-live.ps1" -Mode off -Baseline -ScreenPercentage 66.6667 -Width 3840 -Height 2160 -Fullscreen
if not errorlevel 1 exit /b 0
pause
exit /b 1
