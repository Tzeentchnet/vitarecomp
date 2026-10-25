# SPDX-License-Identifier: GPL-2.0-or-later

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('windows-x64-debug', 'windows-x64-release')]
    [string] $Preset,
    [switch] $Bootstrap
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$configuration = if ($Preset -ceq 'windows-x64-debug') {
    'Debug'
} else {
    'Release'
}
$buildDirectory = Join-Path $root "out\build\$Preset"

function Invoke-Checked {
    param(
        [string] $Label,
        [scriptblock] $Action
    )
    Write-Host "::group::$Label"
    try {
        & $Action
        if ($LASTEXITCODE -ne 0) {
            throw "$Label failed with exit code $LASTEXITCODE."
        }
    } finally {
        Write-Host '::endgroup::'
    }
}

if ($Bootstrap) {
    & (Join-Path $PSScriptRoot 'Install-PinnedTools.ps1') `
        -ToolsRoot (Join-Path $root 'out\ci-tools') `
        -Build |
        Out-Null
}
foreach ($name in @(
    'VITARECOMP_DXC_ROOT',
    'VITARECOMP_VULKAN_HEADERS_ROOT'
)) {
    if ([string]::IsNullOrWhiteSpace(
            [Environment]::GetEnvironmentVariable($name)
        )) {
        throw "$name is unset; use -Bootstrap or provide the pinned dependency."
    }
}

Push-Location $root
try {
    Invoke-Checked -Label "Configure $Preset" -Action {
        & cmake --preset $Preset --fresh
    }
    Invoke-Checked -Label "Build $Preset" -Action {
        & cmake --build --preset $Preset --parallel
    }
    Invoke-Checked -Label 'Verify required build coverage' -Action {
        & (Join-Path $PSScriptRoot 'Test-BuildCoverage.ps1') `
            -BuildDirectory $buildDirectory `
            -Configuration $configuration `
            -Profile Framework
    }
    Invoke-Checked -Label "CTest $Preset" -Action {
        & ctest --preset $Preset --no-tests=error
    }
    Invoke-Checked -Label 'Report Vulkan smoke availability' -Action {
        & ctest `
            --test-dir $buildDirectory `
            -C $configuration `
            -R '^vulkan-conformance$' `
            -V `
            --no-tests=error
    }
} finally {
    Pop-Location
}

Write-Host "Framework Windows build gate passed: $Preset."
