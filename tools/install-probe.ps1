param([string]$GameRoot = 'C:\Program Files (x86)\Steam\steamapps\common\Ghost Master Resurrection')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$bin = Join-Path $GameRoot 'GhostMasterRemake\Binaries\Win64'
$source = Join-Path $root 'build\RelWithDebInfo\ghostmaster_probe.addon64'
$destination = Join-Path $bin 'ghostmaster_probe.addon64'
if (Get-Process 'GhostMasterRemake*' -ErrorAction SilentlyContinue) { throw 'Close the game before installing the probe' }
if (!(Test-Path -LiteralPath $source)) { throw 'Build the probe first' }
if (Test-Path -LiteralPath $destination) { throw 'Destination exists; refusing to overwrite it' }
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$backup = Join-Path $root "backups\before-probe-$stamp"
New-Item -ItemType Directory -Path $backup | Out-Null
$manifest = @()
foreach ($file in (Get-ChildItem -LiteralPath $bin -File)) {
    $manifest += [pscustomobject]@{Name=$file.Name; Length=$file.Length; SHA256=(Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash}
    if ($file.Extension -in '.ini','.log','.cfg') { Copy-Item -LiteralPath $file.FullName -Destination $backup }
}
$manifest | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $backup 'manifest.json') -Encoding utf8
Copy-Item -LiteralPath $source -Destination $destination
if ((Get-FileHash -LiteralPath $source).Hash -ne (Get-FileHash -LiteralPath $destination).Hash) { throw 'Installed hash mismatch' }
Write-Output "Installed new probe only: $destination"
Write-Output "Original file manifest and configuration/log backups: $backup"
