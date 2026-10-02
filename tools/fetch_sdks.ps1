# Downloads the upscaler runtime DLLs into external/*/bin (git-ignored, ~165 MB).
# Headers and import libraries are vendored; only the redistributable DLLs are fetched.
#   DLSS 310.9.1  github.com/NVIDIA/DLSS                      -> external/dlss/bin/nvngx_dlss.dll
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
Get-File 'https://github.com/NVIDIA/DLSS/raw/v310.9.1/lib/Windows_x86_64/rel/nvngx_dlss.dll' (Join-Path $dlssDir 'nvngx_dlss.dll')

# XeSS
$xessZip = Join-Path $tmp 'XeSS_SDK_3.0.2.zip'
Get-File 'https://github.com/intel/xess/releases/download/v3.0.2/XeSS_SDK_3.0.2.zip' $xessZip
Expand-Entries $xessZip @('bin/libxess.dll') (Join-Path $ext 'xess/bin')

# FSR
$ffxZip = Join-Path $tmp 'FidelityFX-Samples-v2.3.0-prebuilt.zip'
Get-File 'https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/releases/download/v2.3.0/FidelityFX-Samples-v2.3.0-prebuilt.zip' $ffxZip
$ffxBase = 'Samples/Upscalers/FidelityFX_FSR/dx12/x64/Release/'
Expand-Entries $ffxZip @(($ffxBase + 'amd_fidelityfx_loader_dx12.dll'), ($ffxBase + 'amd_fidelityfx_upscaler_dx12.dll')) (Join-Path $ext 'ffx/bin')

Write-Host 'done'
