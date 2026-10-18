# Builds every component in the Docker toolchain, then writes the release package to dist\
# and prints its path and build identity. When the build or a check fails, it exits with an
# error and writes no package.
param([string]$Previous)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent

try {
    & (Join-Path $PSScriptRoot 'build.ps1')
} catch {
    [Console]::Error.WriteLine("release: the build failed; no package was written. $_")
    exit 1
}

Push-Location $projectRoot
try {
    $packageArguments = @('tools/release-package.mjs', '--out', 'dist')
    if ($Previous) { $packageArguments += @('--previous', $Previous) }
    node @packageArguments
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
} finally {
    Pop-Location
}
