<#
.SYNOPSIS
    Launches the dashboard, lets it replay, and captures its window to a PNG.

.DESCRIPTION
    Produces the screenshots in the README. Committing them means someone
    deciding whether to build this can see what they would get; regenerating
    them from a script means they can be kept honest when the interface
    changes, rather than slowly becoming a picture of a version that no longer
    exists.

    Needs a real desktop session -- it captures pixels from the screen, so it
    does not work over a service account or a headless runner.

.EXAMPLE
    scripts\capture_screenshot.ps1
    scripts\capture_screenshot.ps1 -Preset release -Seconds 10
#>
[CmdletBinding()]
param(
    [string] $Preset = "release",
    [string] $Output = "docs/screenshots/dashboard.png",
    [int] $Seconds = 8,
    [string] $Data = "data/samples/synthetic_spy_1min.csv"
)

$ErrorActionPreference = 'Stop'

Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;

public static class Win32Window {
    [StructLayout(LayoutKind.Sequential)]
    public struct Rect { public int Left, Top, Right, Bottom; }

    [DllImport("user32.dll")]
    public static extern bool GetWindowRect(IntPtr handle, out Rect rect);

    [DllImport("user32.dll")]
    public static extern bool SetForegroundWindow(IntPtr handle);

    [DllImport("user32.dll")]
    public static extern bool ShowWindow(IntPtr handle, int command);

    // GetWindowRect includes the invisible resize border that DWM draws
    // outside the visible frame, so a capture based on it has a band of
    // whatever is behind the window down two sides. The extended frame bounds
    // are the pixels actually painted.
    [DllImport("dwmapi.dll")]
    public static extern int DwmGetWindowAttribute(
        IntPtr handle, int attribute, out Rect value, int size);

    public const int ExtendedFrameBounds = 9;

    public static Rect VisibleBounds(IntPtr handle) {
        Rect bounds;
        if (DwmGetWindowAttribute(handle, ExtendedFrameBounds, out bounds,
                                  Marshal.SizeOf(typeof(Rect))) == 0) {
            return bounds;
        }
        GetWindowRect(handle, out bounds);
        return bounds;
    }
}
'@

$repo = Split-Path -Parent $PSScriptRoot
Push-Location $repo
try {
    $exe = Join-Path $repo "out/build/$Preset/bin/marketscan.exe"
    if (-not (Test-Path $exe)) {
        throw "No dashboard at $exe. Build the '$Preset' preset first."
    }

    $outputPath = Join-Path $repo $Output
    New-Item -ItemType Directory -Force -Path (Split-Path $outputPath) | Out-Null

    Write-Host "launching $exe"
    $arguments = @('--autoplay', '--data', (Join-Path $repo $Data))
    $process = Start-Process -FilePath $exe -ArgumentList $arguments -PassThru

    # Wait for the window to exist, then for the replay to put something on it.
    #
    # The handle is re-read through Get-Process each time rather than from the
    # original object: Refresh() on a Process captured at Start-Process does not
    # reliably populate MainWindowHandle, and reading a stale empty one produces
    # a null-to-IntPtr conversion error rather than a useful message.
    $handle = [IntPtr]::Zero
    $deadline = (Get-Date).AddSeconds(30)
    while ((Get-Date) -lt $deadline) {
        $live = Get-Process -Id $process.Id -ErrorAction SilentlyContinue
        if ($null -eq $live) {
            throw "The application exited before its window appeared."
        }
        if ($live.MainWindowHandle -ne 0) {
            $handle = $live.MainWindowHandle
            break
        }
        Start-Sleep -Milliseconds 250
    }
    if ($handle -eq [IntPtr]::Zero) {
        $process | Stop-Process -Force
        throw "The window never appeared."
    }

    Write-Host "replaying for $Seconds seconds"
    Start-Sleep -Seconds $Seconds

    [void][Win32Window]::ShowWindow($handle, 9)   # SW_RESTORE
    [void][Win32Window]::SetForegroundWindow($handle)
    Start-Sleep -Milliseconds 900

    $rect = [Win32Window]::VisibleBounds($handle)

    $width = $rect.Right - $rect.Left
    $height = $rect.Bottom - $rect.Top
    if ($width -le 0 -or $height -le 0) {
        $process | Stop-Process -Force
        throw "Could not measure the window."
    }
    Write-Host "capturing ${width}x${height}"

    $bitmap = New-Object System.Drawing.Bitmap $width, $height
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $graphics.CopyFromScreen($rect.Left, $rect.Top, 0, 0, $bitmap.Size)
    $graphics.Dispose()
    $bitmap.Save($outputPath, [System.Drawing.Imaging.ImageFormat]::Png)
    $bitmap.Dispose()

    $process | Stop-Process -Force
    $size = [math]::Round((Get-Item $outputPath).Length / 1KB, 1)
    Write-Host "wrote $Output ($size KB)"
} finally {
    Pop-Location
}
