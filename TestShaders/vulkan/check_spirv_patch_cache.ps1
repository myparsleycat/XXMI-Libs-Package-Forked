param(
    [Parameter(Mandatory = $true)][string]$VcVars,
    [Parameter(Mandatory = $true)][string]$Output,
    [string]$CacheSource = ''
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$vcVarsPath = (Resolve-Path -LiteralPath $VcVars).Path
if (-not $CacheSource) { $CacheSource = Join-Path $root 'DirectX11/vk/SpirvPatchCache.cpp' }
$cacheSourcePath = (Resolve-Path -LiteralPath $CacheSource).Path
New-Item -ItemType Directory -Force -Path $Output | Out-Null
$outDir = (Resolve-Path -LiteralPath $Output).Path
$runDir = Join-Path $outDir ('cache-test-' + [guid]::NewGuid().ToString('N'))
$fixture = Join-Path $runDir 'fixture'
$vk = Join-Path $fixture 'DirectX11/vk'
New-Item -ItemType Directory -Path $vk | Out-Null
New-Item -ItemType Directory -Path (Join-Path $fixture 'DirectX11/dx12') | Out-Null
Copy-Item -LiteralPath $cacheSourcePath -Destination (Join-Path $vk 'SpirvPatchCache.cpp')
foreach ($header in @('SpirvPatchCache.h', 'SpirvPatch.h', 'SpirvTools.h')) {
    Copy-Item -LiteralPath (Join-Path $root "DirectX11/vk/$header") -Destination $vk
}
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'spirv_patch_cache_fixture.h') -Destination $fixture
foreach ($header in @('DirectX11/ShaderRegex.h', 'DirectX11/globals.h',
        'DirectX11/dx12/PipelineRegistry.h', 'DirectX11/vk/VkDump.h')) {
    Set-Content -LiteralPath (Join-Path $fixture $header) -Encoding Ascii `
        -Value '#include "spirv_patch_cache_fixture.h"'
}
$source = Join-Path $PSScriptRoot 'spirv_patch_cache.cpp'
$cache = Join-Path $vk 'SpirvPatchCache.cpp'
$exe = Join-Path $runDir 'spirv_patch_cache.exe'
$command = 'call "{0}" >nul && cl /nologo /std:c++14 /EHsc /W4 /O2 /I"{1}" "{2}" "{3}" /Fe"{4}" /link /INCREMENTAL:NO' -f `
    $vcVarsPath, $fixture, $source, $cache, $exe
Write-Host "Cache source: $cacheSourcePath"
Write-Host "Artifacts: $runDir"
Push-Location -LiteralPath $runDir
try {
    & $env:ComSpec /d /c $command
    if ($LASTEXITCODE -ne 0) { throw 'SPIR-V cache regression build failed' }
    $runtimeLog = Join-Path $runDir 'runtime.log'
    $runCommand = '""{0}" "{1}" > "{2}" 2>&1"' -f $exe, (Join-Path $runDir 'cache'), $runtimeLog
    & $env:ComSpec /d /c $runCommand
    $testExitCode = $LASTEXITCODE
    Get-Content -LiteralPath $runtimeLog
    if ($testExitCode -ne 0) { throw "SPIR-V cache regression failed (native exit $testExitCode)" }
} finally {
    Pop-Location
}
