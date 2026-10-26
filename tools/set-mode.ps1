param([Parameter(Mandatory)][ValidateSet('off','shadow','replace')][string]$Mode,[switch]$Screenshot)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$control = Join-Path $root 'run\control.txt'
if (!(Test-Path -LiteralPath $control)) { throw 'Start tools/launch-live.ps1 first' }
# Write and rename in one directory so the render thread does not read a partially written mode.
$temporary = Join-Path $root 'run\control.next'
$value=$Mode
if($Screenshot) { $value += ' ' + [Guid]::NewGuid().ToString('N') }
Set-Content -LiteralPath $temporary -Value $value -Encoding ascii
Move-Item -LiteralPath $temporary -Destination $control -Force
Write-Output "Requested mode: $Mode (read every 30 presents)"
