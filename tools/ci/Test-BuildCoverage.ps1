# SPDX-License-Identifier: GPL-2.0-or-later

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $BuildDirectory,
    [Parameter(Mandatory = $true)]
    [ValidateSet('Debug', 'RelWithDebInfo', 'Release')]
    [string] $Configuration,
    [ValidateSet('Framework', 'Game')]
    [string] $Profile = 'Framework'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$build = [IO.Path]::GetFullPath($BuildDirectory)
$cachePath = Join-Path $build 'CMakeCache.txt'
if (-not (Test-Path -LiteralPath $cachePath -PathType Leaf)) {
    throw "CMake cache is missing: '$cachePath'."
}
$cache = Get-Content -LiteralPath $cachePath -Raw

$requiredCacheEntries = @(
    'VITARECOMP_BUILD_GPU:BOOL=ON',
    'VITARECOMP_ENABLE_DX12:BOOL=ON',
    'VITARECOMP_ENABLE_VULKAN:BOOL=ON',
    'VITARECOMP_REQUIRE_VULKAN_BACKEND:BOOL=ON',
    'VITARECOMP_DXC_REQUIRED:BOOL=ON',
    'VITARECOMP_DXC_FOUND:BOOL=TRUE',
    'VITARECOMP_VULKAN_BACKEND_FOUND:INTERNAL=TRUE'
)
if ($Profile -ceq 'Framework') {
    $requiredCacheEntries += @(
        'VITARECOMP_BUILD_DX12_CONFORMANCE:BOOL=ON',
        'VITARECOMP_BUILD_VULKAN_CONFORMANCE:BOOL=ON'
    )
}
foreach ($entry in $requiredCacheEntries) {
    if ($cache.IndexOf($entry, [StringComparison]::Ordinal) -lt 0) {
        throw "Required build coverage is absent from CMakeCache.txt: '$entry'."
    }
}

$gpuProject = Get-ChildItem `
    -LiteralPath $build `
    -Filter vitarecomp_gpu.vcxproj `
    -File `
    -Recurse |
    Select-Object -First 1
if ($null -eq $gpuProject) {
    throw 'The vitarecomp_gpu Visual Studio project was not generated.'
}
$gpuProjectText = Get-Content -LiteralPath $gpuProject.FullName -Raw
foreach ($source in @('dx12_backend.cpp', 'vulkan_backend.cpp')) {
    if ($gpuProjectText.IndexOf(
            $source,
            [StringComparison]::OrdinalIgnoreCase
        ) -lt 0) {
        throw "The GPU project does not compile '$source'."
    }
}

$requiredTests = if ($Profile -ceq 'Framework') {
    @(
        'container',
        'inflate',
        'decode',
        'reloc',
        'discover',
        'emit',
        'homebrew-e2e',
        'runtime',
        'runtime-foundation',
        'prior-aot',
        'prior-aot-dispatch-diagnostic',
        'gpu-contract',
        'gpu-c-abi',
        'gpu-dx12-warp-buffer',
        'gpu-vulkan-invalid-icd-no-fallback',
        'gpu-dx12-warp-conformance',
        'vulkan-conformance'
    )
} else {
    @(
        'content-guard-synthetic',
        'content-pipeline-synthetic',
        'generated-staging-synthetic',
        'content-guard-tracked',
        'native-host-unit',
        'native-host-dx12-warp-self-test',
        'native-host-dx12-hardware-self-test',
        'native-host-vulkan-validation-self-test',
        'native-host-unavailable-adapter',
        'native-host-unavailable-backend',
        'native-host-content-validation',
        'native-host-resize-lifecycle-repeat'
    )
}

$testListing = (
    & ctest --test-dir $build -C $Configuration -N 2>&1
) -join "`n"
if ($LASTEXITCODE -ne 0) {
    throw "Unable to enumerate CTest coverage.`n$testListing"
}
foreach ($testName in $requiredTests) {
    if ($testListing -notmatch (
            '(?m)Test\s+#[0-9]+:\s+' + [Regex]::Escape($testName) + '\s*$'
        )) {
        throw "Required CTest coverage is missing: '$testName'."
    }
}

$requiredShaders = if ($Profile -ceq 'Framework') {
    @(
        'dx12_conformance_vs.dxil',
        'dx12_conformance_ps.dxil',
        'vulkan_conformance_vs.spv',
        'vulkan_conformance_ps.spv'
    )
} else {
    @(
        'kzmrecomp_conformance_vs.dxil',
        'kzmrecomp_conformance_ps.dxil',
        'kzmrecomp_conformance_vs.spv',
        'kzmrecomp_conformance_ps.spv'
    )
}
foreach ($shader in $requiredShaders) {
    $matches = @(
        Get-ChildItem `
            -LiteralPath $build `
            -Filter $shader `
            -File `
            -Recurse
    )
    if ($matches.Count -eq 0 -or
        @($matches | Where-Object { $_.Length -gt 0 }).Count -eq 0) {
        throw "Pinned shader-pipeline output is missing or empty: '$shader'."
    }
}

Write-Host (
    "$Profile build coverage passed for ${Configuration}: MSVC /WX, " +
    "DX12, Vulkan, pinned shaders, and $($requiredTests.Count) required tests."
)
