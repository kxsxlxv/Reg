param(
    [string]$Msys2Root = "C:\msys64",
    [string]$BuildDir = "build-netimgui"
)

$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$ucrtRoot = Join-Path $Msys2Root "ucrt64"
$ucrtBin = Join-Path $ucrtRoot "bin"
$ucrtInclude = Join-Path $ucrtRoot "include"
$ucrtLib = Join-Path $ucrtRoot "lib"
$ffmpegRoot = Join-Path $repoRoot ".deps\ffmpeg-ucrt64"
$ffmpegBin = Join-Path $ffmpegRoot "bin"
$buildPath = Join-Path $repoRoot $BuildDir

$requiredPaths = @(
    (Join-Path $ucrtBin "gcc.exe"),
    (Join-Path $ucrtBin "g++.exe"),
    (Join-Path $ucrtBin "cmake.exe"),
    (Join-Path $ucrtBin "ninja.exe"),
    (Join-Path $ucrtBin "glslc.exe"),
    (Join-Path $ucrtInclude "vulkan\vulkan.h"),
    (Join-Path $ucrtLib "libvulkan-1.dll.a"),
    (Join-Path $ffmpegRoot "include\libavcodec\avcodec.h")
)

$missing = @($requiredPaths | Where-Object { -not (Test-Path $_) })
if ($missing.Count -ne 0) {
    $formatted = ($missing | ForEach-Object { "  $_" }) -join [Environment]::NewLine
    throw @"
Reg Windows development environment is incomplete.
Missing:
$formatted

Run once from the repository root:
  powershell -ExecutionPolicy Bypass -File .\scripts\windows\bootstrap-ucrt64.ps1
"@
}

# Keep all compiler, build-output and runtime DLL locations in this PowerShell
# process. A not-yet-created build directory is safe to keep on PATH and becomes
# usable as soon as CMake creates it.
$prepend = @($ffmpegBin, $buildPath, $ucrtBin)
$currentPath = @($env:PATH -split ';' | Where-Object { $_ })
$newPath = [System.Collections.Generic.List[string]]::new()
foreach ($entry in @($prepend + $currentPath)) {
    if ([string]::IsNullOrWhiteSpace($entry)) {
        continue
    }
    if (-not ($newPath | Where-Object { $_ -ieq $entry })) {
        $newPath.Add($entry)
    }
}
$env:PATH = $newPath -join ';'

$env:MSYSTEM = "UCRT64"
$env:CHERE_INVOKING = "1"
$env:MSYS2_PATH_TYPE = "inherit"
$env:CC = Join-Path $ucrtBin "gcc.exe"
$env:CXX = Join-Path $ucrtBin "g++.exe"
$env:FFMPEG_ROOT = $ffmpegRoot
$env:Vulkan_INCLUDE_DIR = $ucrtInclude
$env:Vulkan_LIBRARY = Join-Path $ucrtLib "libvulkan-1.dll.a"
$env:PKG_CONFIG_PATH = Join-Path $ucrtLib "pkgconfig"

Write-Host "Reg UCRT64 environment ready" -ForegroundColor Green
Write-Host "  repo:    $repoRoot"
Write-Host "  MSYS2:   $ucrtRoot"
Write-Host "  FFmpeg:  $ffmpegRoot"
Write-Host "  build:   $buildPath"
Write-Host "  gcc:     $(& (Join-Path $ucrtBin 'gcc.exe') --version | Select-Object -First 1)"
Write-Host "  cmake:   $(& (Join-Path $ucrtBin 'cmake.exe') --version | Select-Object -First 1)"
Write-Host "  ninja:   $(& (Join-Path $ucrtBin 'ninja.exe') --version)"
Write-Host "  glslc:   $(& (Join-Path $ucrtBin 'glslc.exe') --version | Select-Object -First 1)"
