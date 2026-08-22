param(
    [string]$OutputPath = (Join-Path $PSScriptRoot '..\..\SourceArt\T_WoodPixels.png')
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$ResolvedPath = [System.IO.Path]::GetFullPath($OutputPath)
[System.IO.Directory]::CreateDirectory(
    [System.IO.Path]::GetDirectoryName($ResolvedPath)) | Out-Null

$Bitmap = [System.Drawing.Bitmap]::new(
    16, 16, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
try {
    for ($Y = 0; $Y -lt 16; ++$Y) {
        for ($X = 0; $X -lt 16; ++$X) {
            # 纹理以灰度保存，运行时再乘树木的确定性棕色色板。
            # X 主导纵向树皮沟槽，少量 Y 扰动让每个方块不是纯条纹。
            $Band = @(164, 224, 190, 238)[$X % 4]
            $GrainHash = ($X * 17 + $Y * 11 + $X * $Y * 3) % 13
            $Grain = if ($GrainHash -lt 2) {
                -30
            }
            elseif ($GrainHash -gt 10) {
                14
            }
            else {
                0
            }
            # 静态树会剔除相邻体素的内部面；因此边界必须由纹理给出，
            # 否则整根树干即使由方块生成，仍会读成一块连续长木板。
            # 使用棕色明度阶梯而非黑缝，避免重现地形接缝问题。
            $HorizontalEdge = if ($Y -eq 0 -or $Y -eq 15) {
                -62
            }
            elseif ($Y -eq 1 -or $Y -eq 14) {
                -24
            }
            else {
                0
            }
            $VerticalEdge = if ($X -eq 0 -or $X -eq 15) { -18 } else { 0 }
            $Knot = if (
                (($X -ge 5 -and $X -le 7) -and ($Y -ge 6 -and $Y -le 8)) `
                -or (($X -ge 11 -and $X -le 12) -and $Y -eq 11)
            ) {
                -42
            }
            else {
                0
            }
            $Tone = [Math]::Clamp(
                $Band + $Grain + $HorizontalEdge + $VerticalEdge + $Knot,
                104,
                248)
            $Color = [System.Drawing.Color]::FromArgb(
                255, $Tone, $Tone, $Tone)
            $Bitmap.SetPixel($X, $Y, $Color)
        }
    }
    $Bitmap.Save($ResolvedPath, [System.Drawing.Imaging.ImageFormat]::Png)
    Write-Output "WROTE deterministic 16x16 wood texture: $ResolvedPath"
}
finally {
    $Bitmap.Dispose()
}
