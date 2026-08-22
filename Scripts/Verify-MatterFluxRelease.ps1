[CmdletBinding()]
param(
    [string]$EngineRoot = 'C:\Program Files\Epic Games\UE_5.8',
    [string]$AutoSdkRoot = '',
    [string]$UbaRoot = '',
    [string]$RunId = '',
    [ValidateRange(1, 64)]
    [int]$MaxParallelActions = 1,
    [ValidateRange(10, 300)]
    [int]$GameSmokeTimeoutSeconds = 60,
    [switch]$SkipBuild,
    [switch]$SkipServer,
    [switch]$SkipCookStage
)

$ErrorActionPreference = 'Stop'
$ProjectRoot = Split-Path -Parent $PSScriptRoot
$ProjectFile = Join-Path $ProjectRoot 'MatterFlux.uproject'
$BuildBat = Join-Path $EngineRoot 'Engine\Build\BatchFiles\Build.bat'
$RunUat = Join-Path $EngineRoot 'Engine\Build\BatchFiles\RunUAT.bat'
$EditorCmd = Join-Path $EngineRoot 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
$LogBase = Join-Path $ProjectRoot 'Saved\Logs\ReleaseVerification'
$RequiredMatterFluxTests = @(
    'MatterFlux.Project.VersionMetadata',
    'MatterFlux.Project.DefaultMapHasPlayerStart',
    'MatterFlux.Playable.CharacterHas2_5DDefaults',
    'MatterFlux.Playable.ProceduralTerrainDisablesHardwareRayTracing',
    'MatterFlux.Playable.FineTerrainUsesMergedChunkMeshes',
    'MatterFlux.Playable.RandomLevelIsDeterministicAndTraversable',
    'MatterFlux.Playable.WorldActorHasLightingAndCollisionGeometry',
    'MatterFlux.GAS.PlayerStateASCDefaults',
    'MatterFlux.GAS.PlayerAbilityDefaults',
    'MatterFlux.PlayerAbilities.MouseInputMappings',
    'MatterFlux.PlayerAbilities.LeftClickCutAffectsOnlyTargetsAhead',
    'MatterFlux.PlayerAbilities.RightClickFlameIgnitesOnlyTargetsAhead',
    'MatterFlux.PlayerAbilities.VoxelEffects',
    'MatterFlux.Lua.ValidPackRegistersDefinitions',
    'MatterFlux.Lua.FragmentationSettingsAreValidatedAndTransactional',
    'MatterFlux.Lua.SameSourceProducesSameVersionAndDefinitions',
    'MatterFlux.Lua.InvalidReloadKeepsPreviousRegistry',
    'MatterFlux.Lua.SandboxExcludesUnsafeLibraries',
    'MatterFlux.Lua.InstructionBudgetStopsInfiniteScript',
    'MatterFlux.Lua.GameModeUsesReplicatedContentGameState',
    'MatterFlux.Lua.PlayableSceneConsumesActiveDefinitions',
    'MatterFlux.Playable.StreamingCoversEveryAuthorityPlayer',
    'MatterFlux.Playable.StreamingArchivesCombustingAndResidueSources',
    'MatterFlux.Material.AuthoritativeActiveStateRepairsDivergentClient',
    'MatterFlux.Material.ReplicatedSnapshotCompressionIsBoundedAndLossless',
    'MatterFlux.Material.GroundStateChunkIsAtomicBoundedAndLossless',
    'MatterFlux.Combustion.StateSnapshotResumesDeterministically',
    'MatterFlux.Playable.VoxelDecorationsSpawnAsDamageableSources',
    'MatterFlux.Performance.LargeWorldStreamingMovementAndCombustion',
    'MatterFlux.Fragment.Geometry.LShapePreservesArea',
    'MatterFlux.Fragment.Geometry.RingPreservesHole',
    'MatterFlux.Fragment.Geometry.DiagonalTouchProducesStableOuters',
    'MatterFlux.Fragment.Payload.IsFullyDeterministic',
    'MatterFlux.Fragment.Payload.ZeroPowerProducesNoMotion',
    'MatterFlux.Fragment.Payload.RejectsGeometryOutsideReplicationBudget',
    'MatterFlux.Fragment.Mesh.SideNormalsAndIndicesAreValid',
    'MatterFlux.Fragment.Damage.InvalidAndNoChangeRollback',
    'MatterFlux.Fragment.Damage.AllDebrisFilteredStillCommits',
    'MatterFlux.Fragment.Damage.MaterializationFailureRollsBack',
    'MatterFlux.Fragment.Damage.InitializationFailureRollsBack',
    'MatterFlux.Fragment.Support.SupportedRemainderStaysStatic',
    'MatterFlux.Fragment.Support.LuaMinimumDiscardsTinyDetachedComponent',
    'MatterFlux.Fragment.Aggregate.MembersShareOneCarrierActor',
    'MatterFlux.Fragment.Cut.WorldRequestTargetsIntersectingSources',
    'MatterFlux.Fragment.Actor.SourceMaterialPropagatesToFragments',
    'MatterFlux.Fragment.Network.DedicatedServerTwoClients',
    'MatterFlux.Fragment.Network.ListenHostAndThreeClients',
    'MatterFlux.Network.Scale.2Players.NearChunks',
    'MatterFlux.Network.Scale.2Players.FarChunks',
    'MatterFlux.Network.Scale.3Players.NearChunks',
    'MatterFlux.Network.Scale.3Players.FarChunks',
    'MatterFlux.Network.Scale.4Players.NearChunks',
    'MatterFlux.Network.Scale.4Players.FarChunks'
)

if ([string]::IsNullOrWhiteSpace($RunId)) {
    $RunId = Get-Date -Format 'yyyyMMdd-HHmmss'
}
if ($RunId -notmatch '^[A-Za-z0-9._-]+$') {
    throw "RunId may contain only letters, digits, dot, underscore, and dash: $RunId"
}
$LogRoot = Join-Path $LogBase $RunId

if ([string]::IsNullOrWhiteSpace($AutoSdkRoot)) {
    $AutoSdkRoot = $env:UE_SDKS_ROOT
}
if ([string]::IsNullOrWhiteSpace($AutoSdkRoot)) {
    $DocumentsRoot = Split-Path -Parent (Split-Path -Parent $ProjectRoot)
    $ProjectAutoSdk = Join-Path $DocumentsRoot 'unreal-angelscript-forge\.unreal-angelscript-forge\matterflux\cache\toolchains\auto-sdk'
    if (Test-Path -LiteralPath $ProjectAutoSdk -PathType Container) {
        $AutoSdkRoot = $ProjectAutoSdk
    }
}
if (-not [string]::IsNullOrWhiteSpace($AutoSdkRoot)) {
    if (-not (Test-Path -LiteralPath $AutoSdkRoot -PathType Container)) {
        throw "AutoSDK root not found: $AutoSdkRoot"
    }
    $env:UE_SDKS_ROOT = $AutoSdkRoot
}

if ([string]::IsNullOrWhiteSpace($UbaRoot)) {
    $UbaRoot = Join-Path $ProjectRoot 'Saved\UnrealBuildAccelerator'
}
$env:UBA_ROOT = $UbaRoot

# Codex and other restricted shells may omit Windows' common-data variables.
# UE 5.8's UBA fallback dereferences this directory during initialization.
if ([string]::IsNullOrWhiteSpace($env:ProgramData)) {
    $env:ProgramData = Join-Path ([System.IO.Path]::GetPathRoot($ProjectRoot)) 'ProgramData'
}
if ([string]::IsNullOrWhiteSpace($env:ALLUSERSPROFILE)) {
    $env:ALLUSERSPROFILE = $env:ProgramData
}

$RequiredPaths = @($ProjectFile, $BuildBat, $EditorCmd)
if (-not $SkipCookStage) {
    $RequiredPaths += $RunUat
}
foreach ($RequiredPath in $RequiredPaths) {
    if (-not (Test-Path -LiteralPath $RequiredPath -PathType Leaf)) {
        throw "Required file not found: $RequiredPath"
    }
}

if (-not $SkipBuild -and -not $SkipServer) {
    $BaseEngineIni = Join-Path $EngineRoot 'Engine\Config\BaseEngine.ini'
    if (-not (Test-Path -LiteralPath $BaseEngineIni -PathType Leaf)) {
        throw "Engine platform manifest not found: $BaseEngineIni"
    }

    $EngineConfigLines = Get-Content -LiteralPath $BaseEngineIni
    $HasInstalledPlatformInfo = $EngineConfigLines | Where-Object {
        $_ -match '^\s*HasInstalledPlatformInfo\s*=\s*true\s*$'
    }
    if ($HasInstalledPlatformInfo) {
        $DevelopmentWin64ServerConfigs = @($EngineConfigLines | Where-Object {
            $_ -match '^\s*\+InstalledPlatformConfigurations=' `
                -and $_ -match 'PlatformName="Win64"' `
                -and $_ -match 'Configuration="Development"' `
                -and $_ -match 'PlatformType="Server"' `
                -and $_ -match 'Architecture="x64"'
        })
        if ($DevelopmentWin64ServerConfigs.Count -eq 0) {
            throw "Engine distribution does not advertise Win64 Development Server support. Use a UE 5.8 source build or Server-enabled Installed Build; use -SkipServer only for explicitly partial verification."
        }

        $DownloadedServerConfigs = @($DevelopmentWin64ServerConfigs | Where-Object {
            $RequiredFile = [regex]::Match($_, 'RequiredFile="([^"]+)"')
            -not $RequiredFile.Success `
                -or (Test-Path -LiteralPath (Join-Path $EngineRoot $RequiredFile.Groups[1].Value) -PathType Leaf)
        })
        if ($DownloadedServerConfigs.Count -eq 0) {
            throw "Engine distribution advertises Win64 Development Server support, but its required precompiled target file is missing. Install the Server component or use a complete Server-enabled UE 5.8 build."
        }
    }
}

New-Item -ItemType Directory -Path $LogRoot -Force | Out-Null
New-Item -ItemType Directory -Path $UbaRoot -Force | Out-Null

function Invoke-ReleaseStep {
    param(
        [Parameter(Mandatory)]
        [string]$Name,

        [Parameter(Mandatory)]
        [string]$FilePath,

        [Parameter(Mandatory)]
        [string[]]$Arguments
    )

    Write-Host "[$Name] starting..."
    & $FilePath @Arguments
    $ExitCode = $LASTEXITCODE
    if ($ExitCode -ne 0) {
        throw "[$Name] failed with exit code $ExitCode."
    }
    Write-Host "[$Name] passed."
}

function Invoke-ReleaseStepWithOutput {
    param(
        [Parameter(Mandatory)]
        [string]$Name,

        [Parameter(Mandatory)]
        [string]$FilePath,

        [Parameter(Mandatory)]
        [string[]]$Arguments,

        [Parameter(Mandatory)]
        [string]$OutputPath
    )

    Write-Host "[$Name] starting..."
    & $FilePath @Arguments 2>&1 | Tee-Object -FilePath $OutputPath
    $ExitCode = $LASTEXITCODE
    if ($ExitCode -ne 0) {
        throw "[$Name] failed with exit code $ExitCode."
    }
    Write-Host "[$Name] passed."
}

function Invoke-StagedGameSmoke {
    param(
        [Parameter(Mandatory)]
        [string]$StagedExecutable,

        [Parameter(Mandatory)]
        [string]$GameLog,

        [Parameter(Mandatory)]
        [int]$TimeoutSeconds
    )

    if (-not (Test-Path -LiteralPath $StagedExecutable -PathType Leaf)) {
        throw "Staged game executable not found: $StagedExecutable"
    }

    $ExistingProcesses = @(Get-Process -Name 'MatterFlux' -ErrorAction SilentlyContinue)
    if ($ExistingProcesses.Count -gt 0) {
        throw "Refusing staged smoke test while MatterFlux is already running: $($ExistingProcesses.Id -join ', ')"
    }

    $CrashRoot = Join-Path $ProjectRoot 'Saved\Crashes'
    $CrashDirectoriesBefore = @(
        Get-ChildItem -LiteralPath $CrashRoot -Directory -ErrorAction SilentlyContinue |
            Select-Object -ExpandProperty FullName
    )

    $ProcessStartInfo = New-Object System.Diagnostics.ProcessStartInfo
    $ProcessStartInfo.FileName = $StagedExecutable
    $ProcessStartInfo.Arguments = "-Unattended -NullRHI -NoSound -NoSplash -NoP4 -Log -ExecCmds=QUIT -AbsLog=`"$GameLog`""
    $ProcessStartInfo.WorkingDirectory = Split-Path -Parent $StagedExecutable
    $ProcessStartInfo.UseShellExecute = $false
    $ProcessStartInfo.CreateNoWindow = $true

    $GameProcess = New-Object System.Diagnostics.Process
    $GameProcess.StartInfo = $ProcessStartInfo
    if (-not $GameProcess.Start()) {
        throw "Failed to start staged game: $StagedExecutable"
    }

    $GamePid = $GameProcess.Id
    if (-not $GameProcess.WaitForExit($TimeoutSeconds * 1000)) {
        $GameProcess.Kill()
        $GameProcess.WaitForExit()
        throw "Staged game PID $GamePid did not exit within $TimeoutSeconds seconds."
    }
    $GameExitCode = $GameProcess.ExitCode
    $GameProcess.Dispose()

    $CrashDirectoriesAfter = @(
        Get-ChildItem -LiteralPath $CrashRoot -Directory -ErrorAction SilentlyContinue |
            Select-Object -ExpandProperty FullName
    )
    $NewCrashDirectories = @(
        $CrashDirectoriesAfter | Where-Object { $_ -notin $CrashDirectoriesBefore }
    )
    $RemainingProcesses = @(Get-Process -Name 'MatterFlux' -ErrorAction SilentlyContinue)

    if ($GameExitCode -ne 0) {
        throw "Staged game exited with code $GameExitCode. Log: $GameLog"
    }
    if ($NewCrashDirectories.Count -ne 0) {
        throw "Staged game created crash output: $($NewCrashDirectories -join ', ')"
    }
    if ($RemainingProcesses.Count -ne 0) {
        throw "Staged game left MatterFlux processes running: $($RemainingProcesses.Id -join ', ')"
    }
    if (-not (Test-Path -LiteralPath $GameLog -PathType Leaf)) {
        throw "Staged game did not create its log: $GameLog"
    }

    $GameLogText = Get-Content -LiteralPath $GameLog -Raw
    foreach ($FailurePattern in @(
        'Assertion failed:',
        'Fatal error:',
        'Unhandled Exception:',
        'Send to storage server failed',
        'FindPlayerStart: PATHS NOT DEFINED or NO PLAYERSTART',
        'SpawnActor failed because of collision',
        "SpawnDefaultPawnAtTransform: Couldn't spawn Pawn"
    )) {
        if ($GameLogText -match [regex]::Escape($FailurePattern)) {
            throw "Staged game log contains '$FailurePattern'. Log: $GameLog"
        }
    }
    foreach ($RequiredPattern in @(
        'Engine is initialized. Leaving FEngineLoop::Init()',
        'LogExit: Exiting.'
    )) {
        if ($GameLogText -notmatch [regex]::Escape($RequiredPattern)) {
            throw "Staged game log does not contain '$RequiredPattern'. Log: $GameLog"
        }
    }

    Write-Host "[Staged Game Smoke] passed (PID $GamePid, exit code $GameExitCode)."
}

if (-not $SkipBuild) {
    $BuildTargets = @('MatterFluxEditor', 'MatterFlux')
    if (-not $SkipServer) {
        $BuildTargets += 'MatterFluxServer'
    }

    foreach ($Target in $BuildTargets) {
        $BuildLog = Join-Path $LogRoot "$Target.log"
        Invoke-ReleaseStep `
            -Name "$Target Win64 Development" `
            -FilePath $BuildBat `
            -Arguments @(
                $Target,
                'Win64',
                'Development',
                "-Project=$ProjectFile",
                '-WaitMutex',
                '-NoHotReloadFromIDE',
                "-MaxParallelActions=$MaxParallelActions",
                "-Log=$BuildLog"
            )
    }
}

$AutomationLog = Join-Path $LogRoot 'Automation.log'
Invoke-ReleaseStep `
    -Name 'MatterFlux Automation' `
    -FilePath $EditorCmd `
    -Arguments @(
        $ProjectFile,
        '-Unattended',
        '-Multiprocess',
        '-NoCompile',
        '-NullRHI',
        '-NoSplash',
        '-NoSound',
		'-DDC-ForceMemoryCache',
        '-ExecCmds=Automation RunTests MatterFlux',
        '-TestExit=Automation Test Queue Empty',
        "-AbsLog=$AutomationLog"
    )

$AutomationText = Get-Content -LiteralPath $AutomationLog -Raw
if ($AutomationText -match 'Test Completed\. Result=\{Fail') {
    throw 'Automation log contains a failed test.'
}
$SuccessfulMatterFluxTests = [regex]::Matches(
    $AutomationText,
    'Test Completed\. Result=\{Success\} Name=\{[^}]+\} Path=\{(MatterFlux\.[^}]+)\}'
) | ForEach-Object {
    $_.Groups[1].Value
} | Sort-Object -Unique
$DiscoveredTestMatches = [regex]::Matches(
    $AutomationText,
    "Found (?<Count>[0-9]+) automation tests based on 'MatterFlux'"
)
if ($DiscoveredTestMatches.Count -eq 0) {
    throw 'Automation log does not report the number of discovered MatterFlux tests.'
}
$DiscoveredMatterFluxTestCount = [int](
    $DiscoveredTestMatches[$DiscoveredTestMatches.Count - 1].Groups['Count'].Value
)
if ($DiscoveredMatterFluxTestCount -le 0) {
    throw "Automation discovered an invalid MatterFlux test count: $DiscoveredMatterFluxTestCount."
}
if ($SuccessfulMatterFluxTests.Count -ne $DiscoveredMatterFluxTestCount) {
    throw "Automation discovered $DiscoveredMatterFluxTestCount MatterFlux tests, but proves only $($SuccessfulMatterFluxTests.Count) unique successes."
}
foreach ($RequiredMatterFluxTest in $RequiredMatterFluxTests) {
    if ($RequiredMatterFluxTest -notin $SuccessfulMatterFluxTests) {
        throw "Automation log does not prove required test '$RequiredMatterFluxTest' succeeded."
    }
}

$MapCheckLog = Join-Path $LogRoot 'MapCheck.log'
Invoke-ReleaseStep `
    -Name '/Game/Default Map Check' `
    -FilePath $EditorCmd `
    -Arguments @(
        $ProjectFile,
        '/Game/Default',
        '-Unattended',
        '-Multiprocess',
        '-NoCompile',
        '-NullRHI',
        '-NoSplash',
        '-NoSound',
		'-DDC-ForceMemoryCache',
        '-ExecCmds=MAP CHECK,QUIT_EDITOR',
        "-AbsLog=$MapCheckLog"
    )

$MapCheckText = Get-Content -LiteralPath $MapCheckLog -Raw
$MapCheckPassed = $MapCheckText -match 'MapCheck:.*(?:0 errors|0个错误).*(?:0 warnings|0个警告)'
if (-not $MapCheckPassed) {
    throw 'Map Check log does not prove zero errors and zero warnings.'
}

if (-not $SkipCookStage) {
    $StageRoot = Join-Path $ProjectRoot "Saved\StagedBuilds\ReleaseVerification\$RunId"
    $CookStageLog = Join-Path $LogRoot 'CookStage.log'
    Invoke-ReleaseStepWithOutput `
        -Name 'Win64 Development Cook/Stage' `
        -FilePath $RunUat `
        -OutputPath $CookStageLog `
        -Arguments @(
            'BuildCookRun',
            "-project=$ProjectFile",
            '-noP4',
            '-platform=Win64',
            '-clientconfig=Development',
            '-skipbuild',
            '-cook',
            '-stage',
            '-pak',
            '-iostore',
            '-unattended',
            '-utf8output',
            "-stagingdirectory=$StageRoot"
        )

    $StagedProjectStores = @(
        Get-ChildItem -LiteralPath $StageRoot -Recurse -File -Filter 'ue.projectstore' -ErrorAction SilentlyContinue
    )
    if ($StagedProjectStores.Count -ne 0) {
        throw "Staged build unexpectedly depends on live Zen storage: $($StagedProjectStores.FullName -join ', ')"
    }

    $StagedExecutable = Join-Path $StageRoot 'Windows\MatterFlux.exe'
    $GameSmokeLog = Join-Path $LogRoot 'StagedGameSmoke.log'
    Invoke-StagedGameSmoke `
        -StagedExecutable $StagedExecutable `
        -GameLog $GameSmokeLog `
        -TimeoutSeconds $GameSmokeTimeoutSeconds
}

if ($SkipBuild -or $SkipServer -or $SkipCookStage) {
    $SkippedGates = @()
    if ($SkipBuild) {
        $SkippedGates += 'Editor/Game builds'
        if (-not $SkipServer) {
            $SkippedGates += 'Server build'
        }
    }
    elseif ($SkipServer) {
        $SkippedGates += 'Server build'
    }
    if ($SkipCookStage) {
        $SkippedGates += 'Cook/Stage and staged game smoke'
    }
    Write-Host "MatterFlux 0.5.0 partial verification passed; release is not signed. Skipped: $($SkippedGates -join ', '). Logs: $LogRoot"
}
else {
    Write-Host "MatterFlux 0.5.0 release verification passed. Logs: $LogRoot"
}
