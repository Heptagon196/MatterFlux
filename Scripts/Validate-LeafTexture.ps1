param(
    [string]$TexturePath = (Join-Path $PSScriptRoot '..\SourceArt\T_LeafPixels.png')
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$ResolvedPath = [System.IO.Path]::GetFullPath($TexturePath)
if (-not [System.IO.File]::Exists($ResolvedPath)) {
    throw "叶片纹理不存在：$ResolvedPath"
}

$Bitmap = [System.Drawing.Bitmap]::FromFile($ResolvedPath)
try {
    if ($Bitmap.Width -ne 16 -or $Bitmap.Height -ne 16) {
        throw "叶片纹理必须精确为 16x16，当前为 $($Bitmap.Width)x$($Bitmap.Height)"
    }

    $Palette = [System.Collections.Generic.HashSet[string]]::new()
    $TransparentCount = 0
    for ($Y = 0; $Y -lt 16; ++$Y) {
        for ($X = 0; $X -lt 16; ++$X) {
            $Pixel = $Bitmap.GetPixel($X, $Y)
            if ($Pixel.A -ne 0 -and $Pixel.A -ne 255) {
                throw "像素 ($X,$Y) 使用了部分透明 Alpha=$($Pixel.A)"
            }
            if ($Pixel.A -eq 0) {
                ++$TransparentCount
                if ($X -eq 0 -or $X -eq 15 -or $Y -eq 0 -or $Y -eq 15) {
                    throw "边缘像素 ($X,$Y) 透明，会在每个叶块边界形成亮缝"
                }
            }
            else {
                [void]$Palette.Add("$($Pixel.R),$($Pixel.G),$($Pixel.B)")
            }
        }
    }

    if ($Palette.Count -lt 3 -or $Palette.Count -gt 6) {
        throw "叶片不透明色板必须包含 3-6 个离散颜色，当前为 $($Palette.Count)"
    }
    if ($TransparentCount -lt 16 -or $TransparentCount -gt 36) {
        throw "透明孔洞应占 6%-14%，当前为 $TransparentCount/256"
    }

    for ($Index = 0; $Index -lt 16; ++$Index) {
        if ($Bitmap.GetPixel(0, $Index).ToArgb() -ne
            $Bitmap.GetPixel(15, $Index).ToArgb()) {
            throw "左右边缘在第 $Index 行不连续，纹理不能无缝平铺"
        }
        if ($Bitmap.GetPixel($Index, 0).ToArgb() -ne
            $Bitmap.GetPixel($Index, 15).ToArgb()) {
            throw "上下边缘在第 $Index 列不连续，纹理不能无缝平铺"
        }
    }

    Write-Output "PASS 16x16 leaf texture: palette=$($Palette.Count), transparent=$TransparentCount"
}
finally {
    $Bitmap.Dispose()
}
