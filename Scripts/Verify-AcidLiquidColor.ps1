param(
    [Parameter(Mandatory = $true)]
    [string]$Path,

    [int]$Left = 540,
    [int]$Top = 200,
    [int]$Right = 780,
    [int]$Bottom = 480,
    [int]$MinMagentaSamples = 700,
    [int]$MaxMagentaComponents = 3,
    [int]$MinComponentPixels = 32
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$ResolvedPath = (Resolve-Path -LiteralPath $Path).Path
$Bitmap = [System.Drawing.Bitmap]::FromFile($ResolvedPath)
try {
    if ($Left -lt 0 -or $Top -lt 0 -or
        $Right -ge $Bitmap.Width -or $Bottom -ge $Bitmap.Height -or
        $Left -ge $Right -or $Top -ge $Bottom) {
        throw 'The configured acid sample rectangle is outside the screenshot.'
    }

    # 只采样自动截图中酸/水试样的中央区域，避开右侧粉色花朵。
    # 紫红酸液的红、蓝通道都必须显著高于绿色通道；这会让旧的
    # “配置为紫色但被透明混合成灰褐色”截图稳定失败。
    $MagentaSamples = 0
    $RedSum = 0
    $GreenSum = 0
    $BlueSum = 0
    for ($Y = $Top; $Y -le $Bottom; $Y += 2) {
        for ($X = $Left; $X -le $Right; $X += 2) {
            $Color = $Bitmap.GetPixel($X, $Y)
            $LooksLikeVisibleAcid =
                $Color.R -ge 120 -and
                ($Color.R - $Color.G) -ge 45 -and
                ($Color.B - $Color.G) -ge 25
            if ($LooksLikeVisibleAcid) {
                ++$MagentaSamples
                $RedSum += $Color.R
                $GreenSum += $Color.G
                $BlueSum += $Color.B
            }
        }
    }

    if ($MagentaSamples -lt $MinMagentaSamples) {
        throw "FAIL: detected only $MagentaSamples visible magenta-acid samples; required at least $MinMagentaSamples."
    }

    # 全分辨率连通域检查用于阻止液体重新退化成散落的透明方块。
    $SampleWidth = $Right - $Left + 1
    $SampleHeight = $Bottom - $Top + 1
    $MagentaMask = New-Object 'bool[]' ($SampleWidth * $SampleHeight)
    for ($Y = 0; $Y -lt $SampleHeight; ++$Y) {
        for ($X = 0; $X -lt $SampleWidth; ++$X) {
            $Color = $Bitmap.GetPixel($Left + $X, $Top + $Y)
            $MagentaMask[$Y * $SampleWidth + $X] =
                $Color.R -ge 120 -and
                ($Color.R - $Color.G) -ge 45 -and
                ($Color.B - $Color.G) -ge 25
        }
    }

    $Visited = New-Object 'bool[]' $MagentaMask.Length
    $MagentaComponents = 0
    for ($Index = 0; $Index -lt $MagentaMask.Length; ++$Index) {
        if (-not $MagentaMask[$Index] -or $Visited[$Index]) {
            continue
        }
        $ComponentPixels = 0
        $Queue = [System.Collections.Generic.Queue[int]]::new()
        $Queue.Enqueue($Index)
        $Visited[$Index] = $true
        while ($Queue.Count -gt 0) {
            $Current = $Queue.Dequeue()
            ++$ComponentPixels
            $CurrentX = $Current % $SampleWidth
            $CurrentY = [Math]::Floor($Current / $SampleWidth)
            $Neighbors = @(
                ($Current - 1),
                ($Current + 1),
                ($Current - $SampleWidth),
                ($Current + $SampleWidth))
            foreach ($Neighbor in $Neighbors) {
                if ($Neighbor -lt 0 -or $Neighbor -ge $MagentaMask.Length -or
                    $Visited[$Neighbor] -or -not $MagentaMask[$Neighbor]) {
                    continue
                }
                $NeighborX = $Neighbor % $SampleWidth
                $NeighborY = [Math]::Floor($Neighbor / $SampleWidth)
                if ([Math]::Abs($NeighborX - $CurrentX) +
                    [Math]::Abs($NeighborY - $CurrentY) -ne 1) {
                    continue
                }
                $Visited[$Neighbor] = $true
                $Queue.Enqueue($Neighbor)
            }
        }
        # 抗锯齿和透明材质混色会留下几个单像素色点；它们不是独立液滩。
        # 连通性验收只统计足以形成可见物体的连通域。
        if ($ComponentPixels -ge $MinComponentPixels) {
            ++$MagentaComponents
        }
    }
    if ($MagentaComponents -gt $MaxMagentaComponents) {
        throw "FAIL: acid sample is split into $MagentaComponents visible pieces; allowed at most $MaxMagentaComponents."
    }

    $MeanRed = [Math]::Round($RedSum / $MagentaSamples, 1)
    $MeanGreen = [Math]::Round($GreenSum / $MagentaSamples, 1)
    $MeanBlue = [Math]::Round($BlueSum / $MagentaSamples, 1)
    Write-Output "PASS: acid is a continuous magenta surface; $MagentaSamples samples, $MagentaComponents visible component(s), mean RGB=($MeanRed, $MeanGreen, $MeanBlue)."
}
finally {
    $Bitmap.Dispose()
}
