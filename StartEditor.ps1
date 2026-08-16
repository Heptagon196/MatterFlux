param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]] $EditorArgs
)

$ErrorActionPreference = "Stop"

$ProjectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectFile = Join-Path $ProjectRoot "MatterFlux.uproject"

if (-not (Test-Path -LiteralPath $ProjectFile)) {
    Write-Error "Project file not found: $ProjectFile"
    exit 1
}

$Project = Get-Content -LiteralPath $ProjectFile -Raw | ConvertFrom-Json
$EngineAssociation = [string]$Project.EngineAssociation

if ([string]::IsNullOrWhiteSpace($EngineAssociation)) {
    Write-Error "EngineAssociation is missing in $ProjectFile"
    exit 1
}

$EngineRoot = Join-Path "C:\Program Files\Epic Games" ("UE_" + $EngineAssociation)
$EditorExe = Join-Path $EngineRoot "Engine\Binaries\Win64\UnrealEditor.exe"

if (-not (Test-Path -LiteralPath $EditorExe)) {
    Write-Error "UnrealEditor.exe not found: $EditorExe"
    exit 1
}

Write-Host "Starting Unreal Editor:"
Write-Host "  Engine:  $EngineRoot"
Write-Host "  Project: $ProjectFile"

$Arguments = New-Object System.Collections.Generic.List[string]
$Arguments.Add("`"$ProjectFile`"")

if ($null -ne $EditorArgs) {
    foreach ($Arg in $EditorArgs) {
        if (-not [string]::IsNullOrWhiteSpace($Arg)) {
            $Arguments.Add($Arg)
        }
    }
}

Start-Process -FilePath $EditorExe -ArgumentList $Arguments.ToArray() -WorkingDirectory $ProjectRoot
