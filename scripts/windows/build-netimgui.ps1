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

# SDL3 is built by FetchContent and normally lands below _deps/sdl3-build.
# Windows does not search that directory when reg_probe.exe is started from the
# build root, so deploy the fetched runtime next to the executable.
$deployedSdl = Join-Path $buildPath "SDL3.dll"
$sdlCandidate = Get-ChildItem `
    -Path $buildPath `
    -Filter "SDL3.dll" `
    -Recurse `
    -File `
    -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -ine $deployedSdl } |
    Select-Object -First 1

if ($null -eq $sdlCandidate) {
    if (-not (Test-Path $deployedSdl)) {
        throw "SDL3.dll was not produced by the SDL3 FetchContent build."
    }
} else {
    Copy-Item $sdlCandidate.FullName $deployedSdl -Force
    Write-Host "Deployed SDL3 runtime: $($sdlCandidate.FullName) -> $deployedSdl" -ForegroundColor DarkGray
}

# Refresh PATH after deployment so this PowerShell also sees the selected build
# directory and any FetchContent runtime directory discovered by enter-ucrt64.
& $envScript -Msys2Root $Msys2Root -BuildDir $BuildDir

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
Write-Host "  SDL3:  $deployedSdl"
Write-Host ""
Write-Host "Run this build (not build-win) with:"
Write-Host "  .\$BuildDir\reg_probe.exe <arguments>"
