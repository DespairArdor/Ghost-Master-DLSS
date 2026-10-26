param(
    [string]$GameRoot = 'C:\Program Files (x86)\Steam\steamapps\common\Ghost Master Resurrection',
    # Renames other NGX addons (dlss5-feed, renodx-dlss5) to *.disabled; uninstall-addon.ps1 restores them.
    [switch]$DisableConflicting
)
$ErrorActionPreference = 'Stop'
foreach ($module in @('Microsoft.PowerShell.Utility','Microsoft.PowerShell.Management','Microsoft.PowerShell.Security')) {
    Import-Module (Join-Path $PSHOME "Modules\$module\$module.psd1") -ErrorAction Stop
}
$root = Split-Path $PSScriptRoot -Parent
$bin = Join-Path $GameRoot 'GhostMasterRemake\Binaries\Win64'
$exe = Join-Path $bin 'GhostMasterRemake-Win64-Shipping.exe'
$source = Join-Path $root 'build\RelWithDebInfo\ghostmaster_dlss.addon64'
$destination = Join-Path $bin 'ghostmaster_dlss.addon64'
$ini = Join-Path $bin 'ghostmaster_dlss.ini'
$conflicting = @('dlss5-feed.addon64','renodx-dlss5.addon64')
if (Get-Process 'GhostMasterRemake*' -ErrorAction SilentlyContinue) { throw 'Close the game before installing' }
if (!(Test-Path -LiteralPath $source)) { throw 'Build the addon first (tools/build.ps1)' }
if ((Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash.ToLowerInvariant() -ne '0e5e8471808be706d119f37da90c84ecf291c9e91613e5a5f4e9bd4caa8ae4e9') { throw 'Unsupported game build: executable hash differs' }
if (!(Test-Path -LiteralPath (Join-Path $bin 'dxgi.dll'))) { throw 'ReShade (dxgi.dll) is not installed in the game folder' }
$signature = Get-AuthenticodeSignature -LiteralPath (Join-Path $bin 'nvngx_dlss.dll')
if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch 'NVIDIA') { throw 'Game nvngx_dlss.dll is missing or not signed by NVIDIA' }
if (Select-String -LiteralPath (Join-Path $bin 'ReShade.ini') -Pattern '^AddonPath\s*=' -Quiet) { throw 'ReShade.ini sets AddonPath; install the addon there manually' }

$backup = Join-Path $root ('backups\before-install-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Path $backup | Out-Null
$manifest = foreach ($file in (Get-ChildItem -LiteralPath $bin -File)) {
    if ($file.Extension -in '.ini','.cfg') { Copy-Item -LiteralPath $file.FullName -Destination $backup }
    [pscustomobject]@{Name=$file.Name; Length=$file.Length; SHA256=(Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash}
}
$manifest | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $backup 'manifest.json') -Encoding utf8

Copy-Item -LiteralPath $source -Destination $destination -Force
if ((Get-FileHash -LiteralPath $source).Hash -ne (Get-FileHash -LiteralPath $destination).Hash) { throw 'Installed hash mismatch' }
# Frame generation: our pinned helper DLL next to the addon, NVIDIA Streamline runtime (signature-checked) in .\streamline
Copy-Item -LiteralPath (Join-Path $root 'build\RelWithDebInfo\ghostmaster_fg.dll') -Destination $bin -Force
$streamlineSource = Join-Path $root 'external\Streamline\bin\x64'
$streamlineTarget = Join-Path $bin 'streamline'
New-Item -ItemType Directory -Force -Path $streamlineTarget | Out-Null
# nvngx_dlss.dll too: Streamline initializes NGX first and NGX then searches only this folder, so DLSS SR needs its model here as well.
foreach ($name in 'sl.interposer.dll','sl.common.dll','sl.dlss_g.dll','sl.reflex.dll','sl.pcl.dll','nvngx_dlssg.dll','nvngx_dlss.dll') {
    $file = Join-Path $streamlineSource $name
    $sig = Get-AuthenticodeSignature -LiteralPath $file
    if ($sig.Status -ne 'Valid' -or $sig.SignerCertificate.Subject -notmatch 'NVIDIA') { throw "Unverified Streamline runtime: $name" }
    Copy-Item -LiteralPath $file -Destination $streamlineTarget -Force
}
if (!(Test-Path -LiteralPath $ini)) {
@'
[DLSS]
; replace = DLSS on at start, off = game's own TAAU. F6 toggles in game, F7 hides the status line.
Mode=replace
ShowStatus=1
; DLSS model preset: default, J, K (transformer, best quality), L (ultra performance), M (performance).
Preset=default
; game = the game's Resolution Quality slider decides the render scale; addon = ScreenPercentage below, applied from launch.
ScaleSource=game
ScreenPercentage=66.7
; 1 = load DLSS Frame Generation (Streamline) at startup; the menu switch turns it on/off while loaded.
FrameGeneration=1
; Empty = .\streamline next to this file.
StreamlineDir=
; 1 = dump shaders and capture dispatches for research (large logs, slower).
Diagnostics=0
; Empty = nvngx_dlss.dll from this folder. Must be signed by NVIDIA.
RuntimeDir=
; Empty = ghostmaster-dlss folder next to this file.
LogDir=
'@ | Set-Content -LiteralPath $ini -Encoding ascii
}
if ($DisableConflicting) {
    foreach ($name in $conflicting) {
        $path = Join-Path $bin $name
        if (Test-Path -LiteralPath $path) { Rename-Item -LiteralPath $path -NewName "$name.disabled"; Write-Output "Disabled $name" }
    }
} else {
    $present = $conflicting | Where-Object { Test-Path -LiteralPath (Join-Path $bin $_) }
    if ($present) { Write-Warning ("Other NGX addons are active: " + ($present -join ', ') + '. Rerun with -DisableConflicting if DLSS fails or the image breaks.') }
}
Write-Output "Installed: $destination"
Write-Output "Settings: $ini"
Write-Output "Backup of game folder configuration and file manifest: $backup"
