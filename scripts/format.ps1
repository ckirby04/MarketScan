<#
.SYNOPSIS
    Format (or check) every tracked C and C++ source file with clang-format.

.PARAMETER Check
    Report violations and exit non-zero instead of rewriting files. This is
    what CI runs.

.EXAMPLE
    scripts\format.ps1
    scripts\format.ps1 -Check
#>
[CmdletBinding()]
param([switch] $Check)

$ErrorActionPreference = 'Stop'

function Find-ClangFormat {
    $onPath = Get-Command clang-format -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }

    # Visual Studio ships one. Skip the ARM64 build on an x64 host.
    $roots = @(
        (Join-Path $env:ProgramFiles 'Microsoft Visual Studio\2022'),
        (Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\2022')
    ) | Where-Object { $_ -and (Test-Path -LiteralPath $_) }

    foreach ($root in $roots) {
        $hit = Get-ChildItem -LiteralPath $root -Recurse -Filter clang-format.exe -ErrorAction SilentlyContinue |
               Where-Object { $_.FullName -notmatch 'ARM' } |
               Select-Object -First 1
        if ($hit) { return $hit.FullName }
    }
    throw 'clang-format not found. Install it, or add it to PATH.'
}

$clangFormat = Find-ClangFormat
$repoRoot = Split-Path -Parent $PSScriptRoot
Push-Location $repoRoot
try {
    $files = git ls-files '*.cpp' '*.hpp' '*.h' '*.c'
    if (-not $files) { Write-Host 'No sources to format.'; exit 0 }

    & $clangFormat --version
    if ($Check) {
        & $clangFormat --dry-run --Werror --style=file $files
    } else {
        & $clangFormat -i --style=file $files
        Write-Host "Formatted $($files.Count) file(s)."
    }
    exit $LASTEXITCODE
} finally {
    Pop-Location
}
