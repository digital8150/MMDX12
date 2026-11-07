# Downloads the upscaler runtime DLLs into external/*/bin (git-ignored, ~165 MB).
# Headers and import libraries are vendored; only the redistributable DLLs are fetched.
#   DLSS 310.9.16 github.com/NVIDIA/DLSS                      -> external/dlss/bin/nvngx_dlss.dll (+ nvngx_dlssd.dll for RR,
#                                                                 nvngx_dlssg.dll for frame generation)
#   XeSS 3.0.2    github.com/intel/xess (release zip)          -> external/xess/bin/libxess.dll
#   FSR  (FidelityFX SDK 2.3.0 prebuilt samples, release zip)  -> external/ffx/bin/amd_fidelityfx_{loader,upscaler}_dx12.dll
# Usage: powershell -ExecutionPolicy Bypass -File tools\fetch_sdks.ps1   (then rebuild)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$root = Split-Path -Parent $PSScriptRoot
$ext = Join-Path $root 'external'
$tmp = Join-Path ([IO.Path]::GetTempPath()) 'mmdx12_sdks'
New-Item -ItemType Directory -Force $tmp | Out-Null
Add-Type -AssemblyName System.IO.Compression.FileSystem

function Get-File($url, $dest) {
    if (Test-Path $dest) { Write-Host "have $dest"; return }
    Write-Host "download $url"
    Invoke-WebRequest -Uri $url -OutFile $dest -UseBasicParsing
}

function Expand-Entries($zipPath, $entries, $destDir) {
    New-Item -ItemType Directory -Force $destDir | Out-Null
    $zip = [IO.Compression.ZipFile]::OpenRead($zipPath)
    try {
        foreach ($name in $entries) {
            $e = $zip.Entries | Where-Object { $_.FullName -eq $name } | Select-Object -First 1
            if (-not $e) { throw "missing $name in $zipPath" }
            $out = Join-Path $destDir ([IO.Path]::GetFileName($name))
            [IO.Compression.ZipFileExtensions]::ExtractToFile($e, $out, $true)
            Write-Host "  -> $out"
        }
    } finally { $zip.Dispose() }
}

# DLSS (the DLL lives in the repository, served through git LFS media links)
$dlssDir = Join-Path $ext 'dlss/bin'
New-Item -ItemType Directory -Force $dlssDir | Out-Null
Get-File 'https://github.com/NVIDIA/DLSS/raw/v310.9.16/lib/Windows_x86_64/rel/nvngx_dlss.dll' (Join-Path $dlssDir 'nvngx_dlss.dll')
Get-File 'https://github.com/NVIDIA/DLSS/raw/v310.9.16/lib/Windows_x86_64/rel/nvngx_dlssd.dll' (Join-Path $dlssDir 'nvngx_dlssd.dll')
Get-File 'https://github.com/NVIDIA/DLSS/raw/v310.9.16/lib/Windows_x86_64/rel/nvngx_dlssg.dll' (Join-Path $dlssDir 'nvngx_dlssg.dll')

# XeSS
$xessZip = Join-Path $tmp 'XeSS_SDK_3.0.2.zip'
Get-File 'https://github.com/intel/xess/releases/download/v3.0.2/XeSS_SDK_3.0.2.zip' $xessZip
Expand-Entries $xessZip @('bin/libxess.dll') (Join-Path $ext 'xess/bin')

# FSR
$ffxZip = Join-Path $tmp 'FidelityFX-Samples-v2.3.0-prebuilt.zip'
Get-File 'https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/releases/download/v2.3.0/FidelityFX-Samples-v2.3.0-prebuilt.zip' $ffxZip
$ffxBase = 'Samples/Upscalers/FidelityFX_FSR/dx12/x64/Release/'
Expand-Entries $ffxZip @(($ffxBase + 'amd_fidelityfx_loader_dx12.dll'), ($ffxBase + 'amd_fidelityfx_upscaler_dx12.dll')) (Join-Path $ext 'ffx/bin')

# These DLLs ship inside every release: a changed download must never reach a zip. A new SDK version updates this list.
$expected = @{
    'dlss/bin/nvngx_dlss.dll'                  = 'd00b5b0ab29c3ca101b70c3a5ecb3a01b7ef33766a3383a28a18623083e6123e'
    'dlss/bin/nvngx_dlssd.dll'                 = 'b49435ae346f227feafe7bbe937d88277ea0ee7323975ec06488f377fdb46cee'
    'dlss/bin/nvngx_dlssg.dll'                 = '8d59d49b36410dd6b62fef2503a5871d347415bd85d943fa0848563a95dd50c7'
    'xess/bin/libxess.dll'                     = '251659dd84a3e84de67c886a4186e01f3eca49b00641906fe38bb6b807e5d5b7'
    'ffx/bin/amd_fidelityfx_loader_dx12.dll'   = 'e2d85aa05a9bd9ed8b38935fdf5199372cca6f74c12015143bb6f945ee1608aa'
    'ffx/bin/amd_fidelityfx_upscaler_dx12.dll' = 'd0dcccc74a43c44ba435b7a369b456e0970d8a4464e4bd683119b374f2c9fb46'
}
foreach ($rel in $expected.Keys) {
    $file = Join-Path $ext $rel
    $got = (Get-FileHash $file -Algorithm SHA256).Hash.ToLower()
    if ($got -ne $expected[$rel]) {
        Remove-Item -Force $file
        throw "$rel has SHA-256 $got, expected $($expected[$rel]); deleted"
    }
}

Write-Host 'done'
