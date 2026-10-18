$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
node (Join-Path $projectRoot 'tools/component-versions.mjs') generate
if ($LASTEXITCODE -ne 0) { throw 'Component version generation failed.' }
$image = if ($env:BRICKOS_TOOLCHAIN_IMAGE) { $env:BRICKOS_TOOLCHAIN_IMAGE } else { 'brickos-trimui-arm64' }

docker build --tag $image (Join-Path $projectRoot 'toolchain')
if ($LASTEXITCODE -ne 0) { throw 'ARM64 toolchain image build failed.' }

$mount = $projectRoot + ':/workspace'
docker run --rm --volume $mount --workdir /workspace $image sh toolchain/build-container.sh
if ($LASTEXITCODE -ne 0) { throw 'ARM64 C/SDL2 build failed.' }
