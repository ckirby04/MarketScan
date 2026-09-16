<#
.SYNOPSIS
    Run a command inside an MSVC x64 developer environment.

.DESCRIPTION
    Locates Visual Studio with vswhere, imports the environment that
    vcvars64.bat sets up, and then invokes the supplied command.

    Two things this has to get right that vcvars64.bat alone does not:

      1. vcvars64.bat points VCPKG_ROOT at the cut-down vcpkg bundle that
         ships inside Visual Studio. That bundle has no ports tree and
         cannot service a manifest build, so an inherited VCPKG_ROOT is
         preserved across the import instead of being silently replaced.

      2. Visual Studio's bundled CMake and Ninja are not placed on PATH by
         vcvars64.bat. They are appended here, so a copy already on PATH
         keeps priority.

.EXAMPLE
    scripts\dev-env.ps1 cmake --preset release
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [string] $Command,

    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]] $Arguments = @()
)

$ErrorActionPreference = 'Stop'

function Find-VsWhere {
    $candidates = @(
        (Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'),
        (Join-Path $env:ProgramFiles          'Microsoft Visual Studio\Installer\vswhere.exe')
    )
    foreach ($c in $candidates) {
        if ($c -and (Test-Path -LiteralPath $c)) { return $c }
    }
    $onPath = Get-Command vswhere.exe -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }
    throw 'vswhere.exe not found. Is Visual Studio (or its Build Tools) installed?'
}

$vswhere = Find-VsWhere
$vsInstall = & $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath | Select-Object -First 1

if (-not $vsInstall) {
    throw 'No Visual Studio installation with the x64 C++ toolset was found.'
}

$vcvars = Join-Path $vsInstall 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path -LiteralPath $vcvars)) {
    throw "vcvars64.bat not found under '$vsInstall'."
}

# Remember an explicitly chosen vcpkg so the vcvars override can be undone.
$savedVcpkgRoot = $env:VCPKG_ROOT

# Environment variables are process-wide in PowerShell: a script that sets them
# changes its *caller's* environment, not a copy. Without restoring them, every
# invocation from one session appends the toolchain again -- and after a
# handful of calls PATH is long enough that vcvars64.bat fails outright with
# exit code 255. The child process inherits the environment at launch, so
# restoring it afterwards costs nothing.
$savedEnvironment = @{}
foreach ($entry in Get-ChildItem Env:) {
    $savedEnvironment[$entry.Name] = $entry.Value
}

# Import the developer environment. Running `set` after the batch file and
# parsing the result is the only reliable way to lift its variables into this
# process; vcvars64.bat cannot export into a PowerShell session directly.
$envDump = & "$env:ComSpec" /s /c "`"$vcvars`" >nul 2>&1 && set"
if ($LASTEXITCODE -ne 0) {
    throw "vcvars64.bat failed with exit code $LASTEXITCODE."
}

foreach ($line in $envDump) {
    $idx = $line.IndexOf('=')
    if ($idx -gt 0) {
        $name = $line.Substring(0, $idx)
        $value = $line.Substring($idx + 1)
        Set-Item -LiteralPath "Env:$name" -Value $value
    }
}

if ($savedVcpkgRoot) {
    $env:VCPKG_ROOT = $savedVcpkgRoot
}

# Make the bundled build tools reachable without shadowing a system install.
$extraPaths = @(
    (Join-Path $vsInstall 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin'),
    (Join-Path $vsInstall 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja')
)
foreach ($p in $extraPaths) {
    if (Test-Path -LiteralPath $p) { $env:PATH = "$env:PATH;$p" }
}

foreach ($tool in @('cmake', 'ninja', 'cl')) {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        throw "'$tool' is not on PATH after entering the developer environment."
    }
}

try {
    & $Command @Arguments
    $commandExitCode = $LASTEXITCODE
} finally {
    # Put the caller's environment back exactly as it was.
    foreach ($entry in Get-ChildItem Env:) {
        if (-not $savedEnvironment.ContainsKey($entry.Name)) {
            Remove-Item -LiteralPath "Env:$($entry.Name)" -ErrorAction SilentlyContinue
        }
    }
    foreach ($name in $savedEnvironment.Keys) {
        Set-Item -LiteralPath "Env:$name" -Value $savedEnvironment[$name]
    }
}

exit $commandExitCode
