# Builds a Release configuration of MMDX12 in build_release\ and packages a portable zip in dist\.
#   powershell -ExecutionPolicy Bypass -File tools\package_release.ps1 -Version 1.0.0
# The zip holds MMDX12.exe, the shaders and fonts, the runtime DLLs (DXC, upscalers, the VC++ runtime,
# app-local), the licences and a library\ folder skeleton. Run tools\fetch_sdks.ps1 first for the upscalers.
param([Parameter(Mandatory = $true)][string]$Version)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root 'build_release'
$vsDevCmd = 'C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat'
$pip = Join-Path $env:USERPROFILE 'miniconda3\Scripts'

# 1. Release build (no debug info: no PDB path in the exe). The VC++ redist folder comes from the VS environment.
$script = Join-Path ([IO.Path]::GetTempPath()) 'mmdx12_release_build.cmd'
@"
@echo off
call "$vsDevCmd" -arch=x64 -host_arch=x64 >nul || exit /b 1
set PATH=$pip;%PATH%
if exist "$build\CMakeCache.txt" del "$build\CMakeCache.txt"
cmake -S "$root" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release >nul || exit /b 1
cmake --build "$build" --target MMDX12 || exit /b 1
echo VCREDIST=%VCToolsRedistDir%
"@ | Set-Content -Encoding ascii $script
$out = cmd /c $script
if ($LASTEXITCODE -ne 0) { $out; throw 'build failed' }
$redist = ($out | Where-Object { $_ -like 'VCREDIST=*' } | Select-Object -Last 1).Substring(9)
$crt = Get-ChildItem (Join-Path $redist 'x64') -Directory | Where-Object { $_.Name -like 'Microsoft.VC*.CRT' } |
       Select-Object -First 1
if (-not $crt) { throw "VC++ runtime not found under $redist" }

# 2. Stage
$name = "MMDX12-$Version-win64"
$dist = Join-Path $root 'dist'
$stage = Join-Path $dist $name
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force $stage | Out-Null
$bin = Join-Path $build 'bin'
Copy-Item (Join-Path $bin 'MMDX12.exe') $stage
Copy-Item (Join-Path $bin '*.dll') $stage
Copy-Item -Recurse (Join-Path $bin 'shaders') $stage
Copy-Item -Recurse (Join-Path $bin 'assets') $stage
foreach ($dll in 'msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll') {
    Copy-Item (Join-Path $crt.FullName $dll) $stage
}
Copy-Item (Join-Path $root 'LICENSE') (Join-Path $stage 'LICENSE.txt')

$lic = Join-Path $stage 'licenses'
New-Item -ItemType Directory -Force $lic | Out-Null
$third = @{
    'imgui'           = 'external\imgui\LICENSE.txt'
    'stb'             = 'external\stb\LICENSE'
    'miniaudio'       = 'external\miniaudio\LICENSE'
    'DirectX-Headers' = 'external\DirectX-Headers\LICENSE'
    'bullet'          = 'external\bullet\LICENSE.txt'
    'cgltf'           = 'external\cgltf\LICENSE'
    'ufbx'            = 'external\ufbx\LICENSE'
    'NVIDIA-DLSS'     = 'external\dlss\LICENSE.txt'
    'AMD-FidelityFX'  = 'external\ffx\LICENSE.txt'
    'Intel-XeSS'      = 'external\xess\LICENSE.txt'
    'Pretendard'      = 'assets\fonts\Pretendard-LICENSE.txt'
    'Phosphor'        = 'assets\fonts\Phosphor-LICENSE.txt'
    'NotoSansCJK'     = 'assets\fonts\Noto-LICENSE.txt'
}
foreach ($k in $third.Keys) { Copy-Item (Join-Path $root $third[$k]) (Join-Path $lic "$k.txt") }
@'
JSON for Modern C++ 3.12.0 (https://github.com/nlohmann/json)
SPDX-FileCopyrightText: 2013 - 2025 Niels Lohmann <https://nlohmann.me>
SPDX-License-Identifier: MIT (see https://github.com/nlohmann/json/blob/develop/LICENSE.MIT)
'@ | Set-Content -Encoding utf8 (Join-Path $lic 'nlohmann-json.txt')
@'
dxcompiler.dll / dxil.dll: DirectX Shader Compiler from the Windows SDK, redistributable
(https://github.com/microsoft/DirectXShaderCompiler, LICENSE.TXT).
msvcp140.dll / vcruntime140.dll / vcruntime140_1.dll: Microsoft Visual C++ runtime, redistributed app-locally
under the Visual Studio redistribution terms.
'@ | Set-Content -Encoding utf8 (Join-Path $lic 'Microsoft-runtime.txt')

# Suggested library layout (characters/ stages/ songs/ with READMEs); the app creates the same on first run.
$lib = Join-Path $stage 'library'
New-Item -ItemType Directory -Force $lib | Out-Null
Copy-Item -Recurse -Force (Join-Path $root 'assets\library_template\*') $lib

# 3. Zip
$zip = Join-Path $dist "$name.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path $stage -DestinationPath $zip -CompressionLevel Optimal
Write-Host "packaged $zip ($([math]::Round((Get-Item $zip).Length / 1MB, 1)) MB)"
