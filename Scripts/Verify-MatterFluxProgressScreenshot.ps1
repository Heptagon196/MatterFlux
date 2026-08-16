param(
    [Parameter(Mandatory = $true)]
    [string]$Path,

    [Parameter(Mandatory = $true)]
    [ValidateRange(0.0, 1.0)]
    [double]$ExpectedFraction,

    [int]$Left = 607,
    [int]$Right = 992,
    [int]$SampleY = 467,
    [int]$TopBorderY = 460,
    [int]$BottomBorderY = 473,
    [double]$Tolerance = 0.015
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

function Test-BlackPixel {
    param([System.Drawing.Color]$Color)
    return $Color.R -le 5 -and $Color.G -le 5 -and $Color.B -le 5
}

function Test-WhitePixel {
    param([System.Drawing.Color]$Color)
    return $Color.R -ge 250 -and $Color.G -ge 250 -and $Color.B -ge 250
}

$ResolvedPath = (Resolve-Path -LiteralPath $Path).Path
$Bitmap = [System.Drawing.Bitmap]::FromFile($ResolvedPath)
try {
    if ($Left -lt 0 -or $Right -ge $Bitmap.Width -or
        $TopBorderY -lt 0 -or $BottomBorderY -ge $Bitmap.Height -or
        $SampleY -le $TopBorderY -or $SampleY -ge $BottomBorderY) {
        throw 'The configured sample rectangle is outside the screenshot.'
    }

    foreach ($Y in @($TopBorderY, $BottomBorderY)) {
        for ($X = $Left; $X -le $Right; ++$X) {
            if (-not (Test-BlackPixel $Bitmap.GetPixel($X, $Y))) {
                throw "Progress keyline is not solid black at ($X,$Y)."
            }
        }
    }

    $FirstWhite = $null
    for ($X = $Left; $X -le $Right; ++$X) {
        if (Test-WhitePixel $Bitmap.GetPixel($X, $SampleY)) {
            $FirstWhite = $X
            break
        }
        if (-not (Test-BlackPixel $Bitmap.GetPixel($X, $SampleY))) {
            throw "Progress fill contains a non black/white pixel at ($X,$SampleY)."
        }
    }
    if ($null -eq $FirstWhite) {
        throw 'No white track remains; this verifier requires a partial progress sample.'
    }

    $WhiteEnd = $FirstWhite - 1
    for ($X = $FirstWhite; $X -le $Right; ++$X) {
        if (Test-WhitePixel $Bitmap.GetPixel($X, $SampleY)) {
            $WhiteEnd = $X
            continue
        }
        if (Test-BlackPixel $Bitmap.GetPixel($X, $SampleY)) {
            break
        }
        throw "Progress track contains a non black/white pixel at ($X,$SampleY)."
    }

    $FillPixels = $FirstWhite - $Left
    $TrackPixels = $WhiteEnd - $Left + 1
    $MeasuredFraction = $FillPixels / [double]$TrackPixels
    $Difference = [Math]::Abs($MeasuredFraction - $ExpectedFraction)
    if ($Difference -gt $Tolerance) {
        throw ('Progress fill is {0:P2}; expected {1:P2} (tolerance {2:P2}).' -f
            $MeasuredFraction, $ExpectedFraction, $Tolerance)
    }

    Write-Output ('PASS: solid black keyline; {0} black fill pixels + {1} white track pixels = {2:P2}.' -f
        $FillPixels, ($TrackPixels - $FillPixels), $MeasuredFraction)
}
finally {
    $Bitmap.Dispose()
}
