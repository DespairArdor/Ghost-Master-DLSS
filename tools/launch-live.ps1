param(
    [ValidateSet('off','shadow','replace')][string]$Mode = 'shadow',
    [ValidateRange(33.3,99)][double]$ScreenPercentage = 66.6667,
    [ValidateRange(640,8192)][int]$Width = 1280,
    [ValidateRange(360,8192)][int]$Height = 720,
    [switch]$Fullscreen,
    # Shader dumps and periodic dispatch capture; leave off for performance measurements.
    [switch]$Diagnostics,
    # Addon registers only FPS/overlay callbacks: the game's own performance for comparison.
    [switch]$Baseline,
    [string]$GameRoot = 'C:\Program Files (x86)\Steam\steamapps\common\Ghost Master Resurrection'
)
$ErrorActionPreference = 'Stop'
# Resolve built-in modules from this PowerShell installation even when a parent
# application supplies PSModulePath for a different PowerShell version.
foreach ($module in @('Microsoft.PowerShell.Utility','Microsoft.PowerShell.Management','Microsoft.PowerShell.Security')) {
    Import-Module (Join-Path $PSHOME "Modules\$module\$module.psd1") -ErrorAction Stop
}
$root = Split-Path $PSScriptRoot -Parent
if (Get-Process 'GhostMasterRemake*' -ErrorAction SilentlyContinue) { throw 'Close the running game before launching a new test' }
$bin = Join-Path $GameRoot 'GhostMasterRemake\Binaries\Win64'
$exe = Join-Path $bin 'GhostMasterRemake-Win64-Shipping.exe'
$expected = '0e5e8471808be706d119f37da90c84ecf291c9e91613e5a5f4e9bd4caa8ae4e9'
if ((Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash.ToLowerInvariant() -ne $expected) { throw 'Unsupported game build: executable hash differs' }
if (Select-String -LiteralPath (Join-Path $bin 'ReShade.ini') -Pattern '^BasePath\s*=' -Quiet) { throw 'Existing ReShade BasePath overrides the isolated test profile' }
$runtime = Join-Path $root 'external\DLSS\lib\Windows_x86_64\rel'
foreach ($dll in @((Join-Path $runtime 'nvngx_dlss.dll'), (Join-Path $bin 'nvngx_dlss.dll'))) {
    $signature = Get-AuthenticodeSignature -LiteralPath $dll
    if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch 'NVIDIA') { throw "Unverified NGX runtime: $dll" }
}
$profile = Join-Path $root 'run'
$addons = Join-Path $profile 'addons'
New-Item -ItemType Directory -Force -Path $addons | Out-Null
Copy-Item -LiteralPath (Join-Path $root 'build\RelWithDebInfo\ghostmaster_dlss.addon64') -Destination $addons -Force
Copy-Item -LiteralPath (Join-Path $root 'build\RelWithDebInfo\ghostmaster_fg.dll') -Destination $addons -Force
@'
[ADDON]
AddonPath=addons
DisabledAddons=Generic Depth,Effect Runtime Sync

[GENERAL]
EffectSearchPaths=empty
TextureSearchPaths=empty
NoReloadOnInit=1
PresetPath=empty.ini
PerformanceMode=1

[OVERLAY]
TutorialProgress=4
ShowFPS=0
'@ | Set-Content -LiteralPath (Join-Path $profile 'ReShade.ini') -Encoding ascii
$control = Join-Path $profile 'control.txt'
Set-Content -LiteralPath $control -Value $Mode -Encoding ascii
$capture = Join-Path $root ('captures\live-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Path $capture | Out-Null
$env:RESHADE_BASE_PATH_OVERRIDE = $profile
$env:GHOSTMASTER_CONTROL_FILE = $control
$env:GHOSTMASTER_RUNTIME_DIR = $runtime
$env:GHOSTMASTER_CAPTURE_DIR = $capture
$env:GHOSTMASTER_DIAGNOSTICS = if ($Diagnostics) { '1' } else { '0' }
$env:GHOSTMASTER_BASELINE = if ($Baseline) { '1' } else { '0' }
$env:GHOSTMASTER_STREAMLINE_DIR = Join-Path $root 'external\Streamline\bin\x64'
# Visible window is intentional: the user enters a gameplay level for this test.
$percentage = $ScreenPercentage.ToString([System.Globalization.CultureInfo]::InvariantCulture)
$originalSettings = Join-Path $env:LOCALAPPDATA 'GhostMasterRemake\Saved\Config\Windows\GameUserSettings.ini'
$originalSettingsHash = (Get-FileHash -LiteralPath $originalSettings).Hash
# The addon sets r.ScreenPercentage itself. A redirected GameUserSettings copy desynchronized the game's
# Settings.sav slider from the ini the engine reads at startup, so the game's own settings are used unchanged.
$addonSettings = Join-Path $addons 'ghostmaster_dlss.ini'
$text = if (Test-Path -LiteralPath $addonSettings) { Get-Content -LiteralPath $addonSettings -Raw } else { "[DLSS]`r`n" }
foreach ($pair in @(@('ScaleSource','addon'), @('ScreenPercentage',$percentage))) {
    if ($text -match "(?m)^$($pair[0])=") { $text = $text -replace "(?m)^$($pair[0])=.*$", "$($pair[0])=$($pair[1])" }
    else { $text = $text.TrimEnd() + "`r`n$($pair[0])=$($pair[1])`r`n" }
}
Set-Content -LiteralPath $addonSettings -Value $text -Encoding ascii
$windowMode = '-windowed'
if ($Fullscreen) { $windowMode = '-fullscreen' }
$arguments = @($windowMode,"-ResX=$Width","-ResY=$Height",'-nosplash','-dx12')
$process = Start-Process -FilePath $exe -WorkingDirectory $bin -ArgumentList $arguments -PassThru
@{pid=$process.Id;capture=$capture;diagnostics=[bool]$Diagnostics;baseline=[bool]$Baseline;profile=$profile;mode=$Mode;screen_percentage=$ScreenPercentage;width=$Width;height=$Height;exe_sha256=$expected;original_settings_sha256=$originalSettingsHash;addon_settings=$addonSettings} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $profile 'session.json')
Write-Output "Game PID: $($process.Id)"
Write-Output "Capture: $capture"
Write-Output "Control: $control"
