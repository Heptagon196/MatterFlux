param(
    [Parameter(Mandatory = $true)]
    [string] $ScreenshotPath,
    [int] $MinimumWaterPixels = 200,
    [double] $MinimumHorizontalAspect = 2.5
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$resolvedPath = (Resolve-Path -LiteralPath $ScreenshotPath).Path
$bitmap = [System.Drawing.Bitmap]::new($resolvedPath)
try {
    $minimumX = $bitmap.Width
    $minimumY = $bitmap.Height
    $maximumX = -1
    $maximumY = -1
    $waterPixels = 0
    $firstInspectedY = [Math]::Floor($bitmap.Height * 0.18)

    for ($y = $firstInspectedY; $y -lt $bitmap.Height; $y += 1) {
        for ($x = 0; $x -lt $bitmap.Width; $x += 1) {
            $pixel = $bitmap.GetPixel($x, $y)
            $isWaterTint = $pixel.B -ge 70 `
                -and $pixel.B -gt ($pixel.R * 1.12) `
                -and $pixel.G -gt ($pixel.R * 1.08) `
                -and $pixel.B -ge ($pixel.G * 0.82)
            if (-not $isWaterTint) {
                continue
            }
            $waterPixels += 1
            $minimumX = [Math]::Min($minimumX, $x)
            $minimumY = [Math]::Min($minimumY, $y)
            $maximumX = [Math]::Max($maximumX, $x)
            $maximumY = [Math]::Max($maximumY, $y)
        }
    }

    $width = if ($maximumX -ge $minimumX) { $maximumX - $minimumX + 1 } else { 0 }
    $height = if ($maximumY -ge $minimumY) { $maximumY - $minimumY + 1 } else { 0 }
    $aspect = if ($height -gt 0) { $width / [double] $height } else { 0.0 }
    Write-Host ("waterPixels={0} bounds={1}x{2} horizontalAspect={3:N3}" -f `
        $waterPixels, $width, $height, $aspect)

    if ($waterPixels -lt $MinimumWaterPixels) {
        Write-Error "WaterSpray is not visibly rendered on the inspected ground."
        exit 2
    }
    if ($aspect -lt $MinimumHorizontalAspect) {
        Write-Error "WaterSpray remains column-like instead of forming a horizontal puddle."
        exit 3
    }
}
finally {
    $bitmap.Dispose()
}
