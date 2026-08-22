param(
    [string]$OutputPath = (Join-Path $PSScriptRoot '..\..\SourceArt\T_LeafPixels.png')
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$ResolvedPath = [System.IO.Path]::GetFullPath($OutputPath)
[System.IO.Directory]::CreateDirectory(
    [System.IO.Path]::GetDirectoryName($ResolvedPath)) | Out-Null

$Tones = @(220, 238, 255)
$Bitmap = [System.Drawing.Bitmap]::new(
    16, 16, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
try {
    for ($Y = 0; $Y -lt 16; ++$Y) {
        for ($X = 0; $X -lt 16; ++$X) {
            # 最后一行/列复用第一行/列的样本，保证四边严格无缝。
            $TileX = if ($X -eq 15) { 0 } else { $X }
            $TileY = if ($Y -eq 15) { 0 } else { $Y }
            $HoleHash = (
                $TileX * 13 + $TileY * 7 + $TileX * $TileY * 3) % 9
            $IsHole = $X -gt 1 -and $X -lt 14 `
                -and $Y -gt 1 -and $Y -lt 14 `
                -and $HoleHash -eq 0
            if ($IsHole) {
                $Color = [System.Drawing.Color]::FromArgb(0, 0, 0, 0)
            }
            else {
                $ToneHash = (
                    $TileX * $TileX * 3 + $TileY * $TileY * 5 `
                    + $TileX * $TileY * 7 + $TileX * 11 + $TileY * 13) % 11
                $ToneIndex = if ($ToneHash -lt 3) {
                    0
                }
                elseif ($ToneHash -lt 8) {
                    1
                }
                else {
                    2
                }
                $Tone = $Tones[$ToneIndex]
                $Color = [System.Drawing.Color]::FromArgb(255, $Tone, $Tone, $Tone)
            }
            $Bitmap.SetPixel($X, $Y, $Color)
        }
    }
    $Bitmap.Save($ResolvedPath, [System.Drawing.Imaging.ImageFormat]::Png)
    Write-Output "WROTE deterministic 16x16 leaf texture: $ResolvedPath"
}
finally {
    $Bitmap.Dispose()
}
