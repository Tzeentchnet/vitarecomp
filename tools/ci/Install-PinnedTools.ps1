# SPDX-License-Identifier: GPL-2.0-or-later

[CmdletBinding()]
param(
    [string] $ToolsRoot,
    [switch] $Build,
    [switch] $Quality,
    [string] $PythonRequirements,
    [switch] $ExportEnvironment
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if (-not $Build -and -not $Quality -and
    [string]::IsNullOrWhiteSpace($PythonRequirements)) {
    throw 'Select -Build, -Quality, or provide -PythonRequirements.'
}

$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
if ([string]::IsNullOrWhiteSpace($ToolsRoot)) {
    $ToolsRoot = Join-Path $repositoryRoot 'out\ci-tools'
}
$ToolsRoot = [IO.Path]::GetFullPath($ToolsRoot)
[IO.Directory]::CreateDirectory($ToolsRoot) | Out-Null

function Get-VerifiedDownload {
    param(
        [string] $Name,
        [string] $Uri,
        [string] $Sha256
    )

    $downloads = Join-Path $ToolsRoot 'downloads'
    [IO.Directory]::CreateDirectory($downloads) | Out-Null
    $destination = Join-Path $downloads $Name
    $expected = $Sha256.ToLowerInvariant()

    if (Test-Path -LiteralPath $destination -PathType Leaf) {
        $actual = (
            Get-FileHash -LiteralPath $destination -Algorithm SHA256
        ).Hash.ToLowerInvariant()
        if ($actual -ne $expected) {
            Remove-Item -LiteralPath $destination -Force
        }
    }

    if (-not (Test-Path -LiteralPath $destination -PathType Leaf)) {
        Write-Host "Downloading pinned tool: $Name"
        [Net.ServicePointManager]::SecurityProtocol =
            [Net.SecurityProtocolType]::Tls12
        Invoke-WebRequest -UseBasicParsing -Uri $Uri -OutFile $destination
    }

    $actual = (
        Get-FileHash -LiteralPath $destination -Algorithm SHA256
    ).Hash.ToLowerInvariant()
    if ($actual -ne $expected) {
        Remove-Item -LiteralPath $destination -Force
        throw "SHA-256 mismatch for '$Name': expected $expected, got $actual."
    }
    return $destination
}

function Expand-VerifiedZip {
    param(
        [string] $Archive,
        [string] $Destination,
        [string] $Marker
    )

    $markerPath = Join-Path $Destination '.complete'
    if ((Test-Path -LiteralPath $markerPath -PathType Leaf) -and
        (Get-Content -LiteralPath $markerPath -Raw).Trim() -ceq $Marker) {
        return
    }

    if (Test-Path -LiteralPath $Destination) {
        Remove-Item -LiteralPath $Destination -Recurse -Force
    }
    [IO.Directory]::CreateDirectory($Destination) | Out-Null
    Expand-Archive -LiteralPath $Archive -DestinationPath $Destination -Force
    [IO.File]::WriteAllText(
        $markerPath,
        "$Marker`n",
        (New-Object Text.UTF8Encoding($false))
    )
}

function Export-ToolEnvironment {
    param(
        [string] $Name,
        [string] $Value
    )

    [Environment]::SetEnvironmentVariable($Name, $Value, 'Process')
    if ($ExportEnvironment -and
        -not [string]::IsNullOrWhiteSpace($env:GITHUB_ENV)) {
        Add-Content -LiteralPath $env:GITHUB_ENV -Value "$Name=$Value"
    }
}

$result = [ordered]@{}

if ($Build) {
    $dxcVersion = '1.8.2505.32'
    $dxcArchive = Get-VerifiedDownload `
        -Name 'dxc_2025_07_14.zip' `
        -Uri (
            'https://github.com/microsoft/DirectXShaderCompiler/releases/' +
            'download/v1.8.2505.1/dxc_2025_07_14.zip'
        ) `
        -Sha256 '9ad895a6b039e3a8f8c22a1009f866800b840a74b50db9218d13319e215ea8a4'
    $dxcRoot = Join-Path $ToolsRoot "dxc-$dxcVersion"
    Expand-VerifiedZip `
        -Archive $dxcArchive `
        -Destination $dxcRoot `
        -Marker "dxc-$dxcVersion"
    $dxc = Join-Path $dxcRoot 'bin\x64\dxc.exe'
    if (-not (Test-Path -LiteralPath $dxc -PathType Leaf)) {
        throw "Pinned DXC executable is missing from '$dxcRoot'."
    }
    $dxcOutput = (& $dxc --version 2>&1) -join "`n"
    if ($LASTEXITCODE -ne 0 -or
        $dxcOutput -notmatch [Regex]::Escape($dxcVersion)) {
        throw "Pinned DXC did not report version $dxcVersion.`n$dxcOutput"
    }

    $vulkanCommit = '6802bb4733b63ed5efd3adb308a6c885ef180ea1'
    $vulkanArchive = Get-VerifiedDownload `
        -Name "Vulkan-Headers-$vulkanCommit.zip" `
        -Uri (
            'https://codeload.github.com/KhronosGroup/Vulkan-Headers/zip/' +
            $vulkanCommit
        ) `
        -Sha256 '3760505ffbd3589de77e1b9f5ba11eaac13d3c86869588e19f3df3e0fac7e69c'
    $vulkanRoot = Join-Path $ToolsRoot "Vulkan-Headers-$vulkanCommit"
    Expand-VerifiedZip `
        -Archive $vulkanArchive `
        -Destination $vulkanRoot `
        -Marker "vulkan-headers-$vulkanCommit"
    $vulkanHeader = Get-ChildItem `
        -LiteralPath $vulkanRoot `
        -Filter vulkan.h `
        -File `
        -Recurse |
        Where-Object {
            $_.Directory.Name -ceq 'vulkan' -and
            (Test-Path -LiteralPath (
                Join-Path $_.Directory.FullName 'vulkan.hpp'
            ))
        } |
        Select-Object -First 1
    if ($null -eq $vulkanHeader) {
        throw "Pinned Vulkan headers are missing from '$vulkanRoot'."
    }
    $vulkanInclude = Split-Path -Parent $vulkanHeader.Directory.FullName

    Export-ToolEnvironment -Name VITARECOMP_DXC_ROOT -Value $dxcRoot
    Export-ToolEnvironment `
        -Name VITARECOMP_VULKAN_HEADERS_ROOT `
        -Value $vulkanInclude
    $result.DxcRoot = $dxcRoot
    $result.VulkanHeadersRoot = $vulkanInclude
}

if ($Quality) {
    $actionlintVersion = '1.7.7'
    $actionlintArchive = Get-VerifiedDownload `
        -Name "actionlint_${actionlintVersion}_windows_amd64.zip" `
        -Uri (
            "https://github.com/rhysd/actionlint/releases/download/" +
            "v$actionlintVersion/actionlint_${actionlintVersion}_" +
            'windows_amd64.zip'
        ) `
        -Sha256 '7f12f1801bca3d480d67aaf7774f4c2a6359a3ca8eebe382c95c10c9704aa731'
    $actionlintRoot = Join-Path $ToolsRoot "actionlint-$actionlintVersion"
    Expand-VerifiedZip `
        -Archive $actionlintArchive `
        -Destination $actionlintRoot `
        -Marker "actionlint-$actionlintVersion"
    $actionlint = Join-Path $actionlintRoot 'actionlint.exe'
    if (-not (Test-Path -LiteralPath $actionlint -PathType Leaf)) {
        throw "Pinned actionlint is missing from '$actionlintRoot'."
    }
    $reportedActionlintVersion = (& $actionlint -version 2>&1) -join "`n"
    if ($LASTEXITCODE -ne 0 -or
        $reportedActionlintVersion -notmatch [Regex]::Escape(
            $actionlintVersion
        )) {
        throw "Unexpected actionlint version.`n$reportedActionlintVersion"
    }

    $analyzerVersion = '1.25.0'
    $analyzerArchive = Get-VerifiedDownload `
        -Name "PSScriptAnalyzer.$analyzerVersion.nupkg" `
        -Uri (
            'https://github.com/PowerShell/PSScriptAnalyzer/releases/' +
            "download/$analyzerVersion/" +
            "PSScriptAnalyzer.$analyzerVersion.nupkg"
        ) `
        -Sha256 '14e634c828eb98efb9f40b2918ba90f139ed5eccdf663a2a747736d996995d60'
    $analyzerRoot = Join-Path $ToolsRoot "PSScriptAnalyzer-$analyzerVersion"
    $analyzerZip = Join-Path $ToolsRoot "PSScriptAnalyzer.$analyzerVersion.zip"
    Copy-Item -LiteralPath $analyzerArchive -Destination $analyzerZip -Force
    Expand-VerifiedZip `
        -Archive $analyzerZip `
        -Destination $analyzerRoot `
        -Marker "PSScriptAnalyzer-$analyzerVersion"
    Remove-Item -LiteralPath $analyzerZip -Force
    $analyzer = Join-Path $analyzerRoot 'PSScriptAnalyzer.psd1'
    if (-not (Test-Path -LiteralPath $analyzer -PathType Leaf)) {
        throw "Pinned PSScriptAnalyzer is missing from '$analyzerRoot'."
    }

    Export-ToolEnvironment -Name VITARECOMP_ACTIONLINT -Value $actionlint
    Export-ToolEnvironment `
        -Name VITARECOMP_PSSCRIPTANALYZER `
        -Value $analyzer
    $result.Actionlint = $actionlint
    $result.PSScriptAnalyzer = $analyzer
}

if (-not [string]::IsNullOrWhiteSpace($PythonRequirements)) {
    $requirements = [IO.Path]::GetFullPath($PythonRequirements)
    if (-not (Test-Path -LiteralPath $requirements -PathType Leaf)) {
        throw "Python requirements file is missing: '$requirements'."
    }
    $systemPython = Get-Command python -ErrorAction Stop
    $pythonRoot = Join-Path $ToolsRoot 'quality-python'
    $python = Join-Path $pythonRoot 'Scripts\python.exe'
    if (-not (Test-Path -LiteralPath $python -PathType Leaf)) {
        & $systemPython.Source -m venv $pythonRoot
        if ($LASTEXITCODE -ne 0) {
            throw 'Unable to create the pinned quality-tools Python environment.'
        }
    }
    $requirementsHash = (
        Get-FileHash -LiteralPath $requirements -Algorithm SHA256
    ).Hash.ToLowerInvariant()
    $requirementsMarker = Join-Path $pythonRoot '.requirements.sha256'
    $installedHash = if (
        Test-Path -LiteralPath $requirementsMarker -PathType Leaf
    ) {
        (Get-Content -LiteralPath $requirementsMarker -Raw).Trim()
    } else {
        ''
    }
    if ($installedHash -cne $requirementsHash) {
        & $python -m pip install `
            --disable-pip-version-check `
            --requirement $requirements
        if ($LASTEXITCODE -ne 0) {
            throw 'Unable to install pinned Python quality dependencies.'
        }
        [IO.File]::WriteAllText(
            $requirementsMarker,
            "$requirementsHash`n",
            (New-Object Text.UTF8Encoding($false))
        )
    }
    Export-ToolEnvironment -Name VITARECOMP_QUALITY_PYTHON -Value $python
    $result.Python = $python
}

[pscustomobject] $result
