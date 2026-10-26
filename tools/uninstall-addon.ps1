param(
    [string]$GameRoot = 'C:\Program Files (x86)\Steam\steamapps\common\Ghost Master Resurrection',
    # Also delete ghostmaster_dlss.ini (settings) and the ghostmaster-dlss log folder.
    [switch]$RemoveSettings
)
$ErrorActionPreference = 'Stop'
foreach ($module in @('Microsoft.PowerShell.Utility','Microsoft.PowerShell.Management')) {
    Import-Module (Join-Path $PSHOME "Modules\$module\$module.psd1") -ErrorAction Stop
}
$bin = Join-Path $GameRoot 'GhostMasterRemake\Binaries\Win64'
if (Get-Process 'GhostMasterRemake*' -ErrorAction SilentlyContinue) { throw 'Close the game before uninstalling' }
$addon = Join-Path $bin 'ghostmaster_dlss.addon64'
if (Test-Path -LiteralPath $addon) { Remove-Item -LiteralPath $addon; Write-Output "Removed $addon" }
foreach ($path in @((Join-Path $bin 'ghostmaster_fg.dll'),(Join-Path $bin 'streamline'))) {
    if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path -Recurse; Write-Output "Removed $path" }
}
foreach ($name in @('dlss5-feed.addon64','renodx-dlss5.addon64')) {
    $disabled = Join-Path $bin "$name.disabled"
    if ((Test-Path -LiteralPath $disabled) -and !(Test-Path -LiteralPath (Join-Path $bin $name))) { Rename-Item -LiteralPath $disabled -NewName $name; Write-Output "Restored $name" }
}
if ($RemoveSettings) {
    foreach ($path in @((Join-Path $bin 'ghostmaster_dlss.ini'),(Join-Path $bin 'ghostmaster-dlss'))) {
        if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path -Recurse; Write-Output "Removed $path" }
    }
}
