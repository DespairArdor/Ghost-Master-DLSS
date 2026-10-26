@echo off
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\launch-live.ps1" -Mode replace -ScreenPercentage 66.6667 -Width 3840 -Height 2160 -Fullscreen
if not errorlevel 1 exit /b 0
pause
exit /b 1
