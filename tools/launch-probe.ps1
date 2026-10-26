param([switch]$DX11, [string]$GameRoot = 'C:\Program Files (x86)\Steam\steamapps\common\Ghost Master Resurrection')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
if (Get-Process 'GhostMasterRemake*' -ErrorAction SilentlyContinue) { throw 'Game is already running' }
$env:GHOSTMASTER_CAPTURE_DIR = Join-Path $root ('captures\' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
$exe = Join-Path $GameRoot 'GhostMasterRemake\Binaries\Win64\GhostMasterRemake-Win64-Shipping.exe'
$arguments = @('-windowed','-ResX=1280','-ResY=720','-nosplash')
if ($DX11) { $arguments += '-dx11' } else { $arguments += '-dx12' }
# This is the actual interactive game requested for testing, so keep its window visible.
$process = Start-Process -FilePath $exe -WorkingDirectory (Split-Path $exe) -ArgumentList $arguments -PassThru
Write-Output "Game PID: $($process.Id)"
Write-Output "Capture directory: $env:GHOSTMASTER_CAPTURE_DIR"
