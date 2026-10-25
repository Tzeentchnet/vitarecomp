# SPDX-License-Identifier: GPL-2.0-or-later

[CmdletBinding()]
param(
    [string] $BaseRef,
    [switch] $Bootstrap
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
if ($Bootstrap) {
    & (Join-Path $PSScriptRoot 'Install-PinnedTools.ps1') `
        -ToolsRoot (Join-Path $root 'out\ci-tools') `
        -Quality |
        Out-Null
}

& (Join-Path $PSScriptRoot 'Test-StaticQuality.ps1') `
    -RepositoryRoot $root `
    -Profile Framework `
    -BaseRef $BaseRef

$reportDirectory = Join-Path $root 'out\quality-reports'
$reportOne = Join-Path $reportDirectory 'artifact-inventory-a.json'
$reportTwo = Join-Path $reportDirectory 'artifact-inventory-b.json'
& (Join-Path $PSScriptRoot 'Test-ArtifactSafety.ps1') `
    -RepositoryRoot $root `
    -Profile Framework `
    -ReportPath $reportOne
& (Join-Path $PSScriptRoot 'Test-ArtifactSafety.ps1') `
    -RepositoryRoot $root `
    -Profile Framework `
    -ReportPath $reportTwo

$first = [IO.File]::ReadAllBytes($reportOne)
$second = [IO.File]::ReadAllBytes($reportTwo)
if (-not [Linq.Enumerable]::SequenceEqual($first, $second)) {
    throw 'Repeated public artifact inventories were not byte-identical.'
}
Write-Host 'Framework formatting, static quality, and artifact safety passed.'
