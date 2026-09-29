param(
    [string]$Msys2Root = "C:\msys64"
)

$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$bash = Join-Path $Msys2Root "usr\bin\bash.exe"

if (-not (Test-Path $bash)) {
    throw @"
MSYS2 was not found at '$Msys2Root'.

Install the current x86_64 MSYS2 distribution to C:\msys64, then run this
script again. Reg uses the UCRT64 toolchain, matching the Windows CI build.
"@
}

$env:MSYSTEM = "UCRT64"
$env:CHERE_INVOKING = "1"
$env:MSYS2_PATH_TYPE = "inherit"

$escapedRepoRoot = $repoRoot.Replace("'", "'\''")
$repoUnix = (& $bash -lc "cygpath -u '$escapedRepoRoot'").Trim()

if (-not $repoUnix) {
    throw "Failed to convert repository path for MSYS2."
}

Write-Host "Starting Reg UCRT64 bootstrap in $repoRoot"

& $bash -lc "cd '$repoUnix' && exec bash scripts/windows/bootstrap-ucrt64.sh"

if ($LASTEXITCODE -ne 0) {
    throw "Reg UCRT64 bootstrap failed with exit code $LASTEXITCODE."
}
