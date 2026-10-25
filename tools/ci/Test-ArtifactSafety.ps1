# SPDX-License-Identifier: GPL-2.0-or-later

[CmdletBinding()]
param(
    [string] $RepositoryRoot,
    [ValidateSet('Framework', 'Game')]
    [string] $Profile = 'Framework',
    [string] $ReportPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrWhiteSpace($RepositoryRoot)) {
    $RepositoryRoot = Join-Path $PSScriptRoot '..\..'
}
$root = [IO.Path]::GetFullPath($RepositoryRoot)

function Add-Violation {
    param(
        [System.Collections.Generic.List[string]] $List,
        [string] $Path,
        [string] $Reason
    )
    $List.Add("${Path}: $Reason")
}

function Get-TrackedEntries {
    param([string] $Root)

    $lines = @(& git -C $Root ls-files --stage)
    if ($LASTEXITCODE -ne 0) {
        throw "Unable to enumerate tracked files in '$Root'."
    }
    $entries = foreach ($line in $lines) {
        $tab = $line.IndexOf("`t", [StringComparison]::Ordinal)
        if ($tab -lt 0) {
            throw "Unexpected git ls-files output: '$line'."
        }
        $metadata = $line.Substring(0, $tab).Split(' ')
        [pscustomobject]@{
            Mode = $metadata[0]
            Object = $metadata[1]
            Path = $line.Substring($tab + 1).Replace('\', '/')
        }
    }
    return @($entries | Sort-Object -Property Path)
}

$publicFixtures = @{
    'samples/fbdemo/prebuilt/eboot.bin' = @{
        Length = 5121
        Sha256 = 'a6fd165adfbcc25035e79696db20f5da628011a64eb861bc93ab16a9d4e5063e'
    }
    'samples/hello/prebuilt/eboot.bin' = @{
        Length = 52371
        Sha256 = '3f5a3d980dfb323b2c73fe43954f14e9660663d7ea97d94a5ffe5c2014f8ed14'
    }
    'samples/threads/prebuilt/eboot.bin' = @{
        Length = 5446
        Sha256 = 'bf2127d87444771d37d4f6f31cf8871c67d97fb665666664858f571d3f8ba296'
    }
}

$violations = New-Object 'System.Collections.Generic.List[string]'
$inventory = New-Object 'System.Collections.Generic.List[object]'
$fixtureInventory = New-Object 'System.Collections.Generic.List[object]'
$entries = Get-TrackedEntries -Root $root
$maximumRegularFileSize = 1MB
$strictUtf8 = New-Object Text.UTF8Encoding($false, $true)

foreach ($entry in $entries) {
    if ($entry.Mode -ceq '160000') {
        $inventory.Add([pscustomobject][ordered]@{
            path = $entry.Path
            kind = 'submodule'
            gitObject = $entry.Object
        })
        continue
    }
    if ($entry.Mode -notin @('100644', '100755')) {
        Add-Violation `
            -List $violations `
            -Path $entry.Path `
            -Reason "unexpected Git mode $($entry.Mode)"
        continue
    }

    $fullPath = Join-Path $root $entry.Path.Replace('/', '\')
    if (-not (Test-Path -LiteralPath $fullPath -PathType Leaf)) {
        Add-Violation `
            -List $violations `
            -Path $entry.Path `
            -Reason 'tracked file is missing from the worktree'
        continue
    }
    $item = Get-Item -LiteralPath $fullPath -Force
    $isPublicFixture = (
        $Profile -ceq 'Framework' -and
        $publicFixtures.ContainsKey($entry.Path)
    )

    if ($item.Length -gt $maximumRegularFileSize) {
        Add-Violation `
            -List $violations `
            -Path $entry.Path `
            -Reason (
                "unexpected large file ($($item.Length) bytes; limit " +
                "$maximumRegularFileSize)"
            )
    }

    $forbiddenPath = (
        $entry.Path -match (
            '(?i)(?:^|/)(?:work\.bin|eboot\.bin)$|' +
            '\.(?:zip|7z|rar|vpk|pkg|self|elf|velf|suprx|skprx|' +
            'psarc|rif|rap|key|keys|pem|p12|pfx)$|' +
            '^(?:generated|generated-title|recompiled|' +
            'recomp-output|private-data|dumps?|decrypted|extracted)(?:/|$)|' +
            '(?:^|/)src/game/(?:generated|recompiled)(?:/|$)'
        )
    )
    if ($forbiddenPath -and -not $isPublicFixture) {
        Add-Violation `
            -List $violations `
            -Path $entry.Path `
            -Reason 'forbidden private, generated, archive, executable, or key path'
    }

    $bytes = [IO.File]::ReadAllBytes($fullPath)
    if ($isPublicFixture) {
        $expected = $publicFixtures[$entry.Path]
        $sha256 = (
            Get-FileHash -LiteralPath $fullPath -Algorithm SHA256
        ).Hash.ToLowerInvariant()
        if ($item.Length -ne $expected.Length -or
            $sha256 -cne $expected.Sha256) {
            Add-Violation `
                -List $violations `
                -Path $entry.Path `
                -Reason 'public fixture does not match its reviewed size and SHA-256'
        }
        $fixtureInventory.Add([pscustomobject][ordered]@{
            path = $entry.Path
            size = $item.Length
            sha256 = $sha256
            provenance = 'audited public Vita homebrew fixture'
        })
    } else {
        $hasNull = $bytes -contains 0
        $hasElfMagic = (
            $bytes.Length -ge 4 -and
            $bytes[0] -eq 0x7f -and
            $bytes[1] -eq 0x45 -and
            $bytes[2] -eq 0x4c -and
            $bytes[3] -eq 0x46
        )
        $hasSelfMagic = (
            $bytes.Length -ge 4 -and
            $bytes[0] -eq 0x53 -and
            $bytes[1] -eq 0x43 -and
            $bytes[2] -eq 0x45 -and
            $bytes[3] -eq 0
        )
        $hasPsarcMagic = (
            $bytes.Length -ge 4 -and
            $bytes[0] -eq 0x50 -and
            $bytes[1] -eq 0x53 -and
            $bytes[2] -eq 0x41 -and
            $bytes[3] -eq 0x52
        )
        $hasArchiveMagic = (
            $bytes.Length -ge 4 -and
            $bytes[0] -eq 0x50 -and
            $bytes[1] -eq 0x4b -and
            $bytes[2] -in @(0x03, 0x05, 0x07) -and
            $bytes[3] -in @(0x04, 0x06, 0x08)
        )
        if ($hasNull -or $hasElfMagic -or $hasSelfMagic -or
            $hasPsarcMagic -or $hasArchiveMagic) {
            Add-Violation `
                -List $violations `
                -Path $entry.Path `
                -Reason 'unexpected binary or protected-content signature'
        } else {
            try {
                $text = $strictUtf8.GetString($bytes)
            } catch {
                Add-Violation `
                    -List $violations `
                    -Path $entry.Path `
                    -Reason 'text file is not strict UTF-8'
                $text = ''
            }
            $privateAbsolutePathPattern = (
                '(?i)(?:[A-Z]:[\\/](?:Users|Documents and Settings|' +
                'private-data|private|dumps?|decrypted|extracted)(?:[\\/]|$)|' +
                '/(?:home|Users|private|dumps?|decrypted|extracted)/' +
                '[^\s''"<>]+)'
            )
            if ($text -match $privateAbsolutePathPattern) {
                Add-Violation `
                    -List $violations `
                    -Path $entry.Path `
                    -Reason 'contains an absolute private or machine-specific path'
            }
            $normalizedRoot = $root.Replace('\', '/')
            if ($normalizedRoot.Length -gt 3 -and
                $text.Replace('\', '/').IndexOf(
                    $normalizedRoot,
                    [StringComparison]::OrdinalIgnoreCase
                ) -ge 0) {
                Add-Violation `
                    -List $violations `
                    -Path $entry.Path `
                    -Reason 'contains the local repository absolute path'
            }
        }
    }

    $inventory.Add([pscustomobject][ordered]@{
        path = $entry.Path
        kind = if ($isPublicFixture) { 'public-fixture' } else { 'text' }
        size = $item.Length
        gitObject = $entry.Object
    })
}

if ($Profile -ceq 'Framework') {
    foreach ($fixturePath in $publicFixtures.Keys) {
        if ($entries.Path -cnotcontains $fixturePath) {
            Add-Violation `
                -List $violations `
                -Path $fixturePath `
                -Reason 'reviewed public fixture is missing'
        }
    }
    foreach ($required in @(
        'LICENSE',
        'NOTICE',
        'LICENSES/GPL-2.0-or-later.txt',
        'LICENSES/MIT.txt',
        'docs/PROVENANCE.md'
    )) {
        if ($entries.Path -cnotcontains $required) {
            Add-Violation `
                -List $violations `
                -Path $required `
                -Reason 'required license or provenance file is missing'
        }
    }
} else {
    foreach ($required in @(
        'LICENSE',
        'NOTICE',
        'MIGRATION.md',
        'docs/CONTENT_POLICY.md',
        'config/runtime-manifest.example.json'
    )) {
        if ($entries.Path -cnotcontains $required) {
            Add-Violation `
                -List $violations `
                -Path $required `
                -Reason 'required license, provenance, or policy file is missing'
        }
    }
    $guard = Join-Path $root 'tools\content\Guard-PrivateContent.ps1'
    if (-not (Test-Path -LiteralPath $guard -PathType Leaf)) {
        Add-Violation `
            -List $violations `
            -Path 'tools/content/Guard-PrivateContent.ps1' `
            -Reason 'complete private-content guard is missing'
    } else {
        & $guard -RepositoryRoot $root -Tracked
        if ($LASTEXITCODE -ne 0) {
            throw 'The complete private-content guard failed.'
        }
    }
}

if ($violations.Count -gt 0) {
    [Console]::Error.WriteLine(
        "Artifact safety rejected $($violations.Count) item(s):"
    )
    foreach ($violation in @($violations | Sort-Object -Unique)) {
        [Console]::Error.WriteLine("  $violation")
    }
    throw 'Tracked repository content failed artifact-safety policy.'
}

$fixtureReportEntries = @(
    $fixtureInventory |
        ForEach-Object { $_ } |
        Sort-Object -Property path
)
$inventoryReportEntries = @(
    $inventory | ForEach-Object { $_ }
)
$report = [pscustomobject][ordered]@{
    schemaVersion = 1
    profile = $Profile
    trackedEntryCount = $entries.Count
    maximumRegularFileSize = $maximumRegularFileSize
    publicFixtures = $fixtureReportEntries
    entries = $inventoryReportEntries
}
$json = ($report | ConvertTo-Json -Depth 8) + "`n"
$jsonRepeat = ($report | ConvertTo-Json -Depth 8) + "`n"
if ($json -cne $jsonRepeat) {
    throw 'Artifact inventory serialization was not deterministic.'
}

if (-not [string]::IsNullOrWhiteSpace($ReportPath)) {
    $reportFullPath = [IO.Path]::GetFullPath($ReportPath)
    [IO.Directory]::CreateDirectory(
        (Split-Path -Parent $reportFullPath)
    ) | Out-Null
    [IO.File]::WriteAllText(
        $reportFullPath,
        $json,
        (New-Object Text.UTF8Encoding($false))
    )
    $reportHash = (
        Get-FileHash -LiteralPath $reportFullPath -Algorithm SHA256
    ).Hash.ToLowerInvariant()
    Write-Host "Deterministic artifact inventory SHA-256: $reportHash"
}

Write-Host (
    "Artifact safety passed: $($entries.Count) tracked entries, " +
    "$($fixtureInventory.Count) reviewed public fixture(s), no private data."
)
