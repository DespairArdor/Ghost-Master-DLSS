param([string]$GameRoot = 'C:\Program Files (x86)\Steam\steamapps\common\Ghost Master Resurrection')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$manifest = Get-Content -LiteralPath (Join-Path $root 'backups\before-probe-20261025-092046\manifest.json') -Raw | ConvertFrom-Json
if ($manifest.Name -contains 'ghostmaster_probe.addon64') { throw 'Refusing cleanup: probe existed in the original manifest' }
$path = Join-Path $GameRoot 'GhostMasterRemake\Binaries\Win64\ghostmaster_probe.addon64'
if (!(Test-Path -LiteralPath $path)) { Write-Output 'Diagnostic addon already absent'; return }
$expected = '639521D79C571D9179B36A2E9569D8EF535902AA3F8E5CEF06A141A463AA4A8E'
$actual = (Get-FileHash -LiteralPath $path).Hash
if ($actual -ne $expected) { throw "Refusing cleanup: unexpected probe hash $actual" }
$backup = Join-Path $root 'backups\installed-probe-v3.addon64'
Copy-Item -LiteralPath $path -Destination $backup -Force
if ((Get-FileHash -LiteralPath $backup).Hash -ne $actual) { throw 'Backup verification failed' }
Remove-Item -LiteralPath $path
Write-Output "Removed only our diagnostic addon; retained verified backup at $backup"
