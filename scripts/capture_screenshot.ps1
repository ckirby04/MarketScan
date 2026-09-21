<#
.SYNOPSIS
    Launches the dashboard, lets it replay, and captures its window to a PNG.

.DESCRIPTION
    Produces the screenshots in the README. Committing them means someone
    deciding whether to build this can see what they would get; regenerating
    them from a script means they can be kept honest when the interface
    changes, rather than slowly becoming a picture of a version that no longer
    exists.

    The application captures itself: this launches it with --screenshot, it
    replays, reads its own back buffer, writes a BMP and exits, and this converts
    that to PNG. So the window does not have to be in front, unobstructed, or
    even looked at -- which is what makes a screenshot a command anybody can
    repeat rather than a window somebody arranged.

    Still needs a desktop session, because the window has to exist and be
    rendering; this is not headless. And it refuses to write a capture that came
    back blank rather than committing a white rectangle, which is what the
    desktop-reading version used to do when it could not raise the window.

.EXAMPLE
    scripts\capture_screenshot.ps1
    scripts\capture_screenshot.ps1 -Preset release -Seconds 10

.EXAMPLE
    # One panel, over a universe that churns.
    scripts\capture_screenshot.ps1 -Data data/cache/synthetic/bars.csv `
        -Output docs/screenshots/universe.png -ExtraArgs @(
            '--universe', 'data/cache/synthetic/membership.csv', '--focus', 'Universe')
#>
[CmdletBinding()]
param(
    [string] $Preset = "release",
    [string] $Output = "docs/screenshots/dashboard.png",
    [int] $Seconds = 8,
    [string] $Data = "data/samples/synthetic_spy_1min.csv",

    # Anything else to hand the application: --universe, --focus, --view.
    # Passed through verbatim so a screenshot of a particular panel or view mode
    # is a command anybody can repeat rather than a window somebody arranged.
    [string[]] $ExtraArgs = @()
)

$ErrorActionPreference = 'Stop'

Add-Type -AssemblyName System.Drawing

$repo = Split-Path -Parent $PSScriptRoot
Push-Location $repo
try {
    $exe = Join-Path $repo "out/build/$Preset/bin/marketscan.exe"
    if (-not (Test-Path $exe)) {
        throw "No dashboard at $exe. Build the '$Preset' preset first."
    }

    $outputPath = Join-Path $repo $Output
    New-Item -ItemType Directory -Force -Path (Split-Path $outputPath) | Out-Null

    $bmp = [System.IO.Path]::ChangeExtension(
        (Join-Path ([System.IO.Path]::GetTempPath()) ([System.IO.Path]::GetRandomFileName())), ".bmp")

    Write-Host "launching $exe"
    $arguments = @('--autoplay',
                   '--data', (Join-Path $repo $Data),
                   '--screenshot', $bmp,
                   '--screenshot-after', $Seconds) + $ExtraArgs
    Write-Host ("arguments: " + ($arguments -join ' '))
    $process = Start-Process -FilePath $exe -ArgumentList $arguments -PassThru

    # The application replays, captures its own back buffer and exits. Nothing
    # here has to find the window, raise it, or read the desktop -- see
    # apps/marketscan/screenshot.hpp for why that matters.
    if (-not $process.WaitForExit(($Seconds + 60) * 1000)) {
        $process | Stop-Process -Force
        throw "The application did not finish within $($Seconds + 60) seconds."
    }
    if ($process.ExitCode -ne 0) {
        throw "The application exited with code $($process.ExitCode)."
    }
    if (-not (Test-Path $bmp)) {
        throw "The application did not write $bmp. Was it built with the GUI enabled?"
    }

    $outputPath = Join-Path $repo $Output
    New-Item -ItemType Directory -Force -Path (Split-Path $outputPath) | Out-Null

    $bitmap = New-Object System.Drawing.Bitmap $bmp
    try {
        Write-Host ("captured {0}x{1}" -f $bitmap.Width, $bitmap.Height)

        # Checked rather than trusted. A capture that comes back one flat colour
        # is what an unrendered window looks like, and before this check existed
        # exactly that was written out and committed as though it were a
        # screenshot. Sampled on a grid: enough to tell a drawn window from an
        # empty one without walking two million pixels in PowerShell.
        $distinct = @{}
        for ($x = 4; $x -lt $bitmap.Width; $x += 41) {
            for ($y = 4; $y -lt $bitmap.Height; $y += 37) {
                $distinct[$bitmap.GetPixel($x, $y).ToArgb()] = $true
            }
        }
        if ($distinct.Count -lt 8) {
            throw ("The capture came back nearly blank ($($distinct.Count) distinct colours). " +
                   "The window rendered nothing. Raise -Seconds, or check that the run has " +
                   "data to show.")
        }

        $bitmap.Save($outputPath, [System.Drawing.Imaging.ImageFormat]::Png)
    } finally {
        $bitmap.Dispose()
        Remove-Item $bmp -ErrorAction SilentlyContinue
    }

    $size = [math]::Round((Get-Item $outputPath).Length / 1KB, 1)
    Write-Host "wrote $Output ($size KB)"
} finally {
    Pop-Location
}
