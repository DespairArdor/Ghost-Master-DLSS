param([string]$Version = '1.0.0')
# Builds and packages the Nexus Mods release into release\:
#   release\GhostMasterDLSS-<version>\      staging folder (archive contents)
#   release\GhostMasterDLSS-<version>.zip   upload file, extracts into the game folder
#   release\SHA256SUMS.txt, release\nexus\  page texts
$ErrorActionPreference = 'Stop'
foreach ($module in @('Microsoft.PowerShell.Utility','Microsoft.PowerShell.Management','Microsoft.PowerShell.Security','Microsoft.PowerShell.Archive')) {
    Import-Module (Join-Path $PSHOME "Modules\$module\$module.psd1") -ErrorAction Stop
}
$root = Split-Path $PSScriptRoot -Parent
& (Join-Path $PSScriptRoot 'build.ps1')
$build = Join-Path $root 'build\RelWithDebInfo'
foreach ($file in 'ghostmaster_dlss.addon64','ghostmaster_fg.dll') {
    $actual = (Get-Item -LiteralPath (Join-Path $build $file)).VersionInfo.FileVersion
    if ($actual -ne $Version) { throw "$file has version $actual, expected $Version (src\version.rc)" }
}

$release = Join-Path $root 'release'
$name = "GhostMasterDLSS-$Version"
$stage = Join-Path $release $name
$zip = Join-Path $release "$name.zip"
foreach ($path in @($stage, $zip, (Join-Path $release 'nexus'), (Join-Path $release 'SHA256SUMS.txt'))) {
    if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path -Recurse -Force }
}
$win64 = Join-Path $stage 'GhostMasterRemake\Binaries\Win64'
$streamline = Join-Path $win64 'streamline'
New-Item -ItemType Directory -Force -Path $streamline | Out-Null

$sources = Join-Path $root 'release-src'
Copy-Item -LiteralPath (Join-Path $build 'ghostmaster_dlss.addon64') -Destination $win64
Copy-Item -LiteralPath (Join-Path $build 'ghostmaster_fg.dll') -Destination $win64
Copy-Item -LiteralPath (Join-Path $sources 'ghostmaster_dlss.ini') -Destination $win64
Copy-Item -LiteralPath (Join-Path $sources 'README.txt') -Destination $stage
Copy-Item -LiteralPath (Join-Path $sources 'CHANGELOG.txt') -Destination $stage

# NVIDIA runtime: Streamline 2.14.16 and DLSS 310.9.16, only files with a valid NVIDIA signature.
$runtime = Join-Path $root 'external\Streamline\bin\x64'
foreach ($file in 'sl.interposer.dll','sl.common.dll','sl.dlss_g.dll','sl.reflex.dll','sl.pcl.dll','nvngx_dlssg.dll','nvngx_dlss.dll') {
    $path = Join-Path $runtime $file
    $signature = Get-AuthenticodeSignature -LiteralPath $path
    if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch 'NVIDIA') { throw "Unverified NVIDIA runtime: $file" }
    Copy-Item -LiteralPath $path -Destination $streamline
}
Copy-Item -LiteralPath (Join-Path $root 'external\DLSS\LICENSE.txt') -Destination (Join-Path $streamline 'NVIDIA_RTX_SDK_LICENSE.txt')
Copy-Item -LiteralPath (Join-Path $root 'external\Streamline\license.txt') -Destination (Join-Path $streamline 'Streamline_LICENSE.txt')
Copy-Item -LiteralPath (Join-Path $root 'external\Streamline\3rd-party-licenses.md') -Destination (Join-Path $streamline 'Streamline_3rd-party-licenses.md')
Copy-Item -LiteralPath (Join-Path $sources 'nexus') -Destination (Join-Path $release 'nexus') -Recurse

# Not Compress-Archive: Windows PowerShell 5.1 writes '\' into entry names, which mod managers and other unzippers mishandle.
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$archive = [System.IO.Compression.ZipFile]::Open($zip, [System.IO.Compression.ZipArchiveMode]::Create)
try {
    foreach ($file in Get-ChildItem -LiteralPath $stage -Recurse -File) {
        $entry = $file.FullName.Substring($stage.Length + 1).Replace('\', '/')
        [void][System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $file.FullName, $entry, [System.IO.Compression.CompressionLevel]::Optimal)
    }
} finally { $archive.Dispose() }
$sums = foreach ($file in Get-ChildItem -LiteralPath $stage -Recurse -File) {
    '{0}  {1}' -f (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant(), $file.FullName.Substring($stage.Length + 1)
}
$sums += '{0}  {1}' -f (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant(), "$name.zip"
$sums | Set-Content -LiteralPath (Join-Path $release 'SHA256SUMS.txt') -Encoding ascii
Write-Output "Release folder: $release"
Write-Output ("Upload file: {0} ({1:N1} MB)" -f $zip, ((Get-Item -LiteralPath $zip).Length / 1MB))
