param(
    [Parameter(Mandatory=$true)][string]$EngineRoot,
    [string]$SourceRoot,
    [string]$Owner = 'SNAPKITTYWEST',
    [int]$Port = 38473,
    [switch]$SkipBuild
)
$ErrorActionPreference = 'Stop'
$RepoRoot = Split-Path $PSScriptRoot -Parent
if (!$SourceRoot) { $SourceRoot = $RepoRoot }
$SourceRoot = (Resolve-Path -LiteralPath $SourceRoot).Path
$Editor = Join-Path $EngineRoot 'Engine\Binaries\Win64\UnrealEditor.exe'
if (!(Test-Path -LiteralPath $Editor)) { throw "Unreal Editor not found: $Editor" }
$Dotnet = Join-Path $env:USERPROFILE '.dotnet\dotnet.exe'
if (!(Test-Path -LiteralPath $Dotnet)) { $Dotnet = (Get-Command dotnet).Source }
$WorldDir = Join-Path $RepoRoot 'out\world3d'
New-Item -ItemType Directory -Force -Path $WorldDir | Out-Null
if (!$SkipBuild) {
    & $Dotnet build (Join-Path $RepoRoot 'Repoverse.sln') -warnaserror
    if ($LASTEXITCODE) { throw 'Core build failed' }
    & (Join-Path $RepoRoot 'unreal\Prepare-Unreal.ps1') -EngineRoot $EngineRoot
}
$Cli = Join-Path $RepoRoot 'src\Repoverse.Cli\bin\Debug\net8.0\Repoverse.Cli.dll'
$Snapshot = Join-Path $WorldDir 'snapshot.json'
$Manifest = Join-Path $WorldDir 'manifest.json'
& $Dotnet $Cli ingest-local $SourceRoot --owner $Owner --out $Snapshot
if ($LASTEXITCODE) { throw 'Local ingestion failed' }
& $Dotnet $Cli generate --snapshot $Snapshot --out $Manifest
if ($LASTEXITCODE) { throw 'World generation failed' }
$Data = Get-Content -LiteralPath $Manifest -Raw | ConvertFrom-Json
$Repo = $Data.buildings[0].repo
$Token = [Guid]::NewGuid().ToString('N') + [Guid]::NewGuid().ToString('N')
$Sidecar = Join-Path $RepoRoot 'src\Repoverse.Sidecar\bin\Debug\net8.0\Repoverse.Sidecar.dll'
$Save = Join-Path $WorldDir 'save.json'
# ArgumentList is a command-line string on Windows; quote every path with spaces.
$ServiceArgs = @(('"'+$Sidecar+'"'), '--manifest', ('"'+$Manifest+'"'), '--token', $Token, '--port', $Port, '--save', ('"'+$Save+'"'), '--source-root', ('"'+$SourceRoot+'"'), '--source-repo', $Repo)
$Service = Start-Process -FilePath $Dotnet -ArgumentList $ServiceArgs -PassThru -WindowStyle Hidden -RedirectStandardOutput (Join-Path $WorldDir 'service.log') -RedirectStandardError (Join-Path $WorldDir 'service-error.log')
try {
    $Ready = $false
    for ($i=0; $i -lt 100; $i++) {
        if ($Service.HasExited) { throw 'World service exited; inspect out/world3d/service-error.log' }
        try { $null = Invoke-RestMethod "http://127.0.0.1:$Port/v1/health" -Headers @{'X-Repoverse-Token'=$Token}; $Ready=$true; break } catch { Start-Sleep -Milliseconds 100 }
    }
    if (!$Ready) { throw 'World service did not become ready' }
    $Project = Join-Path $RepoRoot 'unreal\Repoverse\Repoverse.uproject'
    $GameArgs = @(('"'+$Project+'"'), '/Game/Maps/Repoverse?game=/Script/Repoverse.RepoverseWorldMode', '-game', "-RepoverseToken=$Token", "-RepoversePort=$Port", '-windowed', '-ResX=1440', '-ResY=900')
    $Game = Start-Process -FilePath $Editor -ArgumentList $GameArgs -PassThru -Wait
    if ($Game.ExitCode) { throw "Unreal exited with code $($Game.ExitCode)" }
} finally {
    if (!$Service.HasExited) { Stop-Process -Id $Service.Id }
}
