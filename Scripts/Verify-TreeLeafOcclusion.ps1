param(
    [Parameter(Mandatory = $true)]
    [string]$Path,

    [int]$Left = 430,
    [int]$Top = 140,
    [int]$Right = 850,
    [int]$Bottom = 470,
    [int]$MaxWoodSamples = 10
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$ResolvedPath = (Resolve-Path -LiteralPath $Path).Path
$Bitmap = [System.Drawing.Bitmap]::FromFile($ResolvedPath)
try {
    if ($Left -lt 0 -or $Top -lt 0 -or
        $Right -ge $Bitmap.Width -or $Bottom -ge $Bitmap.Height -or
        $Left -ge $Right -or $Top -ge $Bottom) {
        throw 'The configured tree-crown sample rectangle is outside the screenshot.'
    }

    $WoodSamples = 0
    for ($Y = $Top; $Y -le $Bottom; $Y += 2) {
        for ($X = $Left; $X -le $Right; $X += 2) {
            $Color = $Bitmap.GetPixel($X, $Y)
            $LooksLikeWood =
                $Color.R -ge 115 -and
                $Color.G -ge 55 -and $Color.G -le 185 -and
                $Color.B -le 90 -and
                ($Color.R - $Color.G) -ge 20
            if ($LooksLikeWood) {
                ++$WoodSamples
            }
        }
    }

    if ($WoodSamples -gt $MaxWoodSamples) {
        throw "FAIL: detected $WoodSamples wood-colored samples inside the leaf-crown occlusion region; allowed $MaxWoodSamples."
    }
    Write-Output "PASS: leaf crown occludes wood; $WoodSamples wood-colored samples (allowed $MaxWoodSamples)."
}
finally {
    $Bitmap.Dispose()
}
