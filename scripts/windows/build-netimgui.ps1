param(
    [string]$Msys2Root = "C:\msys64",
    [string]$BuildDir = "build-netimgui",
    [switch]$Clean,
    [switch]$SkipTests
)

$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$buildPath = Join-Path $repoRoot $BuildDir
$envScript = Join-Path $PSScriptRoot "enter-ucrt64.ps1"

if ($Clean -and (Test-Path $buildPath)) {
    Write-Host "Removing $buildPath" -ForegroundColor Yellow
    Remove-Item -Recurse -Force $buildPath
}

& $envScript -Msys2Root $Msys2Root -BuildDir $BuildDir

$ucrtRoot = Join-Path $Msys2Root "ucrt64"
$ucrtBin = Join-Path $ucrtRoot "bin"
$cmake = Join-Path $ucrtBin "cmake.exe"
$ninja = Join-Path $ucrtBin "ninja.exe"
$gcc = Join-Path $ucrtBin "gcc.exe"
$gxx = Join-Path $ucrtBin "g++.exe"
$ffmpegRoot = $env:FFMPEG_ROOT

Write-Host ""
Write-Host "Configuring Reg ($BuildDir)" -ForegroundColor Cyan
& $cmake `
    -S $repoRoot `
    -B $buildPath `
    -G Ninja `
    -DCMAKE_BUILD_TYPE=RelWithDebInfo `
    "-DCMAKE_MAKE_PROGRAM=$ninja" `
    "-DCMAKE_C_COMPILER=$gcc" `
    "-DCMAKE_CXX_COMPILER=$gxx" `
    "-DFFMPEG_ROOT=$ffmpegRoot" `
    -DREG_ENABLE_VALIDATION=OFF `
    -DREG_BUILD_TESTS=ON `
    -DREG_ENABLE_NETIMGUI_REMOTE=ON `
    "-DVulkan_INCLUDE_DIR=$env:Vulkan_INCLUDE_DIR" `
    "-DVulkan_LIBRARY=$env:Vulkan_LIBRARY"

if ($LASTEXITCODE -ne 0) {
    throw "CMake configure failed with exit code $LASTEXITCODE."
}

Write-Host ""
Write-Host "Building Reg" -ForegroundColor Cyan
& $cmake --build $buildPath --parallel
if ($LASTEXITCODE -ne 0) {
    throw "CMake build failed with exit code $LASTEXITCODE."
}

if (-not $SkipTests) {
    Write-Host ""
    Write-Host "Running tests" -ForegroundColor Cyan
    & $cmake -E env `
        "PATH=$env:PATH" `
        ctest --test-dir $buildPath --output-on-failure
    if ($LASTEXITCODE -ne 0) {
        throw "CTest failed with exit code $LASTEXITCODE."
    }
}

Write-Host ""
Write-Host "Reg NetImgui Windows build is ready" -ForegroundColor Green
Write-Host "  build: $buildPath"
Write-Host "  exe:   $(Join-Path $buildPath 'reg_probe.exe')"
Write-Host ""
Write-Host "For the current PowerShell session, refresh Reg paths with:"
Write-Host "  & .\scripts\windows\enter-ucrt64.ps1 -BuildDir $BuildDir"
