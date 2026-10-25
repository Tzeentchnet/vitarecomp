# SPDX-License-Identifier: GPL-2.0-or-later

[CmdletBinding()]
param(
    [string] $RepositoryRoot,
    [ValidateSet('Framework', 'Game')]
    [string] $Profile = 'Framework',
    [string] $ActionlintPath = $env:VITARECOMP_ACTIONLINT,
    [string] $PSScriptAnalyzerPath = $env:VITARECOMP_PSSCRIPTANALYZER,
    [string] $BaseRef
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrWhiteSpace($RepositoryRoot)) {
    $RepositoryRoot = Join-Path $PSScriptRoot '..\..'
}
$root = [IO.Path]::GetFullPath($RepositoryRoot)

function Invoke-Checked {
    param(
        [string] $Label,
        [scriptblock] $Action
    )
    & $Action
    if ($LASTEXITCODE -ne 0) {
        throw "$Label failed with exit code $LASTEXITCODE."
    }
}

function Get-ComparisonBase {
    param(
        [string] $Root,
        [string] $Requested
    )

    $configuredCandidates = @(
        @($Requested, $env:QUALITY_BASE_SHA) |
            Where-Object { -not [string]::IsNullOrWhiteSpace($_) }
    )
    if ($configuredCandidates.Count -gt 0) {
        $candidates = @($configuredCandidates)
    } else {
        $status = @(& git -C $Root status --porcelain)
        if ($LASTEXITCODE -ne 0) {
            throw 'Unable to inspect Git status for the formatting base.'
        }
        $candidates = if ($status.Count -gt 0) { @('HEAD') } else { @('HEAD^') }
    }
    foreach ($candidate in $candidates) {
        if ([string]::IsNullOrWhiteSpace($candidate)) {
            continue
        }
        & git -C $Root cat-file -e "${candidate}^{commit}" 2>$null
        if ($LASTEXITCODE -eq 0) {
            return $candidate
        }
    }
    return ''
}

function Test-MarkdownLinks {
    param(
        [string] $Root,
        [string[]] $Files
    )

    $failures = New-Object 'System.Collections.Generic.List[string]'
    foreach ($relative in $Files) {
        $fullPath = Join-Path $Root $relative.Replace('/', '\')
        $text = Get-Content -LiteralPath $fullPath -Raw
        $matches = [Regex]::Matches($text, '\[[^\]]+\]\(([^)\s]+)(?:\s+"[^"]*")?\)')
        foreach ($match in $matches) {
            $target = $match.Groups[1].Value.Trim('<', '>')
            if ($target -match '^(?:https?://|mailto:|#)') {
                continue
            }
            $target = $target.Split('#')[0].Split('?')[0]
            if ([string]::IsNullOrWhiteSpace($target)) {
                continue
            }
            $target = [Uri]::UnescapeDataString($target)
            $resolved = Join-Path (Split-Path -Parent $fullPath) $target
            if (-not (Test-Path -LiteralPath $resolved)) {
                $failures.Add("$relative -> $($match.Groups[1].Value)")
            }
        }
    }
    if ($failures.Count -gt 0) {
        throw (
            "Broken relative Markdown links:`n  " +
            (@($failures | Sort-Object -Unique) -join "`n  ")
        )
    }
}

function Find-ClangFormatTools {
    $clangFormat = Get-Command clang-format -ErrorAction SilentlyContinue
    $gitClangFormat = Get-Command git-clang-format -ErrorAction SilentlyContinue
    if ($null -ne $clangFormat -and $null -ne $gitClangFormat) {
        return [pscustomobject]@{
            ClangFormat = $clangFormat.Source
            GitClangFormat = $gitClangFormat.Source
        }
    }

    $vswhere = Join-Path ${env:ProgramFiles(x86)} (
        'Microsoft Visual Studio\Installer\vswhere.exe'
    )
    if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
        return $null
    }
    $installation = (
        & $vswhere -latest -products * -property installationPath
    ) | Select-Object -First 1
    if ([string]::IsNullOrWhiteSpace($installation)) {
        return $null
    }
    $llvmRoot = Join-Path $installation 'VC\Tools\Llvm\x64\bin'
    $clangFormatPath = Join-Path $llvmRoot 'clang-format.exe'
    $gitClangFormatPath = Join-Path $llvmRoot 'git-clang-format'
    if ((Test-Path -LiteralPath $clangFormatPath -PathType Leaf) -and
        (Test-Path -LiteralPath $gitClangFormatPath -PathType Leaf)) {
        return [pscustomobject]@{
            ClangFormat = $clangFormatPath
            GitClangFormat = $gitClangFormatPath
        }
    }
    return $null
}

if (-not (Test-Path -LiteralPath $ActionlintPath -PathType Leaf)) {
    throw 'Pinned actionlint is unavailable; run Install-PinnedTools.ps1 -Quality.'
}
if (-not (Test-Path -LiteralPath $PSScriptAnalyzerPath -PathType Leaf)) {
    throw (
        'Pinned PSScriptAnalyzer is unavailable; run ' +
        'Install-PinnedTools.ps1 -Quality.'
    )
}

$base = Get-ComparisonBase -Root $root -Requested $BaseRef
if (-not [string]::IsNullOrWhiteSpace($base)) {
    Invoke-Checked -Label 'git diff --check (committed range)' -Action {
        & git -C $root diff --check "$base..HEAD" --
    }
}
Invoke-Checked -Label 'git diff --check (worktree)' -Action {
    & git -C $root diff --check --
}
Invoke-Checked -Label 'git diff --check (index)' -Action {
    & git -C $root diff --cached --check --
}

$tracked = @(& git -C $root ls-files)
if ($LASTEXITCODE -ne 0) {
    throw 'Unable to enumerate tracked files for static quality checks.'
}
if ($Profile -ceq 'Game') {
    $tracked = @($tracked | Where-Object {
        -not $_.StartsWith(
            'third_party/',
            [StringComparison]::OrdinalIgnoreCase
        )
    })
}

$jsonFiles = @($tracked | Where-Object { $_ -match '(?i)\.json$' })
foreach ($relative in $jsonFiles) {
    $fullPath = Join-Path $root $relative.Replace('/', '\')
    try {
        Get-Content -LiteralPath $fullPath -Raw |
            ConvertFrom-Json -ErrorAction Stop |
            Out-Null
    } catch {
        throw "Invalid JSON '$relative': $($_.Exception.Message)"
    }
}

$textQualityFiles = @($tracked | Where-Object {
    $_ -match '(?i)(?:\.cmake|\.json|\.md|\.ps1|\.psd1|\.ya?ml)$' -or
    (Split-Path -Leaf $_) -ceq 'CMakeLists.txt'
})
foreach ($relative in $textQualityFiles) {
    $fullPath = Join-Path $root $relative.Replace('/', '\')
    $text = Get-Content -LiteralPath $fullPath -Raw
    if ($text.Length -gt 0 -and -not $text.EndsWith("`n")) {
        throw "Text file does not end with a newline: '$relative'."
    }
    if ($text -match '(?m)[ `t]+$') {
        throw "Text file contains trailing whitespace: '$relative'."
    }
}

$markdownFiles = @($tracked | Where-Object { $_ -match '(?i)\.md$' })
Test-MarkdownLinks -Root $root -Files $markdownFiles

Push-Location $root
try {
    Invoke-Checked -Label 'CMake preset validation' -Action {
        & cmake --list-presets
    }
} finally {
    Pop-Location
}

$workflowFiles = @($tracked | Where-Object {
    $_ -match '^\.github/workflows/[^/]+\.ya?ml$'
})
if ($workflowFiles.Count -eq 0) {
    throw 'No GitHub Actions workflow is tracked.'
}
foreach ($relative in $workflowFiles) {
    Invoke-Checked -Label "actionlint ($relative)" -Action {
        & $ActionlintPath (Join-Path $root $relative.Replace('/', '\'))
    }
}

Import-Module $PSScriptAnalyzerPath -Force
$analyzerSettings = Join-Path $PSScriptRoot 'PSScriptAnalyzerSettings.psd1'
$powerShellFiles = @($tracked | Where-Object { $_ -match '(?i)\.ps1$' })
$diagnostics = @()
foreach ($relative in $powerShellFiles) {
    $diagnostics += @(
        Invoke-ScriptAnalyzer `
            -Path (Join-Path $root $relative.Replace('/', '\')) `
            -Settings $analyzerSettings
    )
}
if ($diagnostics.Count -gt 0) {
    $rendered = $diagnostics |
        Sort-Object ScriptName, Line, RuleName |
        Format-Table ScriptName, Line, RuleName, Message -AutoSize |
        Out-String
    throw "PSScriptAnalyzer reported diagnostics:`n$rendered"
}

if (-not [string]::IsNullOrWhiteSpace($base)) {
    $changedSources = @(
        & git -C $root diff --name-only --diff-filter=ACMR "$base..HEAD" -- `
            '*.c' '*.h' '*.cpp' '*.hpp'
    )
    if ($LASTEXITCODE -ne 0) {
        throw 'Unable to enumerate changed C/C++ sources.'
    }
    if ($changedSources.Count -gt 0) {
        $formatTools = Find-ClangFormatTools
        if ($null -eq $formatTools) {
            throw 'clang-format is unavailable for changed C/C++ formatting.'
        }
        $python = (Get-Command python -ErrorAction Stop).Source
        $arguments = @(
            $formatTools.GitClangFormat,
            '--binary',
            $formatTools.ClangFormat,
            '--diff',
            $base,
            '--'
        ) + $changedSources
        Push-Location $root
        try {
            $formatDiff = (& $python @arguments 2>&1) -join "`n"
            if ($LASTEXITCODE -ne 0) {
                throw "git-clang-format failed:`n$formatDiff"
            }
        } finally {
            Pop-Location
        }
        if ($formatDiff -match '(?m)^diff --git ') {
            throw "Changed C/C++ lines are not clang-formatted:`n$formatDiff"
        }
    }
}

$requiredFiles = if ($Profile -ceq 'Framework') {
    @(
        'LICENSE',
        'NOTICE',
        'LICENSES/GPL-2.0-or-later.txt',
        'LICENSES/MIT.txt',
        'docs/PROVENANCE.md'
    )
} else {
    @(
        'LICENSE',
        'NOTICE',
        'MIGRATION.md',
        'docs/CONTENT_POLICY.md',
        'config/framework.commit'
    )
}
foreach ($relative in $requiredFiles) {
    $fullPath = Join-Path $root $relative.Replace('/', '\')
    if (-not (Test-Path -LiteralPath $fullPath -PathType Leaf) -or
        (Get-Item -LiteralPath $fullPath).Length -eq 0) {
        throw "Required license/provenance input is missing or empty: '$relative'."
    }
}

Write-Host (
    "Static quality passed: $($jsonFiles.Count) JSON, " +
    "$($markdownFiles.Count) Markdown, $($powerShellFiles.Count) PowerShell, " +
    "$($workflowFiles.Count) workflow file(s)."
)
