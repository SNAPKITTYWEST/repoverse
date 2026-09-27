param(
    [Parameter(Mandatory=$true)][string]$EngineRoot,
    [switch]$Launch
)
$ErrorActionPreference = 'Stop'
$RepoRoot = Split-Path $PSScriptRoot -Parent
$UnifyRoot = Join-Path $RepoRoot 'unify'
$ProjectRoot = Join-Path $PSScriptRoot 'Repoverse'
$Editor = Join-Path $EngineRoot 'Engine\Binaries\Win64\UnrealEditor.exe'
$BuildTool = Join-Path $EngineRoot 'Engine\Build\BatchFiles\Build.bat'
if (!(Test-Path -LiteralPath $Editor)) { throw "Unreal Editor not found: $Editor" }
$env:UNIFY_ROOT = $UnifyRoot
$BuildRoot = Join-Path $UnifyRoot 'build-unreal'
& cmake -S $UnifyRoot -B $BuildRoot -G 'Visual Studio 17 2022' -A x64 -DUNIFY_NO_GLOBAL_NEW=ON
if ($LASTEXITCODE) { throw 'CMake configure failed; install the matching Visual Studio C++ toolchain.' }
& cmake --build $BuildRoot --config Release --target unify_engine lua unify_assets
if ($LASTEXITCODE) { throw 'UNIFY build failed' }
$env:UNIFY_BUILD = Join-Path $BuildRoot 'Release'
$Plugin = Join-Path $ProjectRoot 'Plugins\UnifyUnreal'
New-Item -ItemType Directory -Force -Path $Plugin | Out-Null
Copy-Item -Path (Join-Path $UnifyRoot 'backends\unreal\*') -Destination $Plugin -Recurse -Force
$RuntimeAssets = Join-Path $ProjectRoot 'Unify\assets'
New-Item -ItemType Directory -Force -Path $RuntimeAssets | Out-Null
Copy-Item -Path (Join-Path $UnifyRoot 'game\assets\*') -Destination $RuntimeAssets -Recurse -Force
$ProjectFile = Join-Path $ProjectRoot 'Repoverse.uproject'
& $BuildTool RepoverseEditor Win64 Development "-Project=$ProjectFile" -WaitMutex
if ($LASTEXITCODE) { throw 'Unreal build failed' }
$EditorCmd = Join-Path $EngineRoot 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
$MapScript = Join-Path $PSScriptRoot 'create-map.py'
& $EditorCmd $ProjectFile "-ExecutePythonScript=$MapScript" -unattended -nullrhi -nosound
if ($LASTEXITCODE) { throw 'Map creation failed' }
if ($Launch) { & $Editor $ProjectFile '/Game/Maps/Repoverse' -game }
