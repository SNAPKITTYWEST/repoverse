param([switch]$Play)
$ErrorActionPreference = 'Stop'
$RepoRoot = Split-Path $PSScriptRoot -Parent
$BuildRoot = Join-Path $RepoRoot 'unify\build-game'
$VendorRoot = Join-Path $BuildRoot 'vendor'
New-Item -ItemType Directory -Force -Path $VendorRoot | Out-Null
$Archive = Join-Path $VendorRoot 'SDL2-devel-2.32.10-mingw.zip'
if (!(Test-Path -LiteralPath $Archive)) {
    Invoke-WebRequest 'https://github.com/libsdl-org/SDL/releases/download/release-2.32.10/SDL2-devel-2.32.10-mingw.zip' -OutFile $Archive
}
$Expected = 'F15CFF5FCA62EC9381A016EF1D42A95C638CD72D2F226BA5781C76FE43DBD1AC'
if ((Get-FileHash -LiteralPath $Archive -Algorithm SHA256).Hash -ne $Expected) { throw 'SDL archive checksum mismatch' }
$SdlRoot = Join-Path $VendorRoot 'sdl2\SDL2-2.32.10\x86_64-w64-mingw32'
if (!(Test-Path -LiteralPath $SdlRoot)) { Expand-Archive -LiteralPath $Archive -DestinationPath (Join-Path $VendorRoot 'sdl2') }
& cmake -S (Join-Path $RepoRoot 'unify') -B $BuildRoot -G 'MinGW Makefiles' -DCMAKE_BUILD_TYPE=Release "-DSDL2_DIR=$SdlRoot/lib/cmake/SDL2"
if ($LASTEXITCODE) { throw 'Configure failed' }
& cmake --build $BuildRoot -j 6
if ($LASTEXITCODE) { throw 'Build failed' }
Copy-Item -LiteralPath (Join-Path $SdlRoot 'bin\SDL2.dll') -Destination (Join-Path $BuildRoot 'SDL2.dll') -Force
& ctest --test-dir $BuildRoot --output-on-failure
if ($LASTEXITCODE) { throw 'Game checks failed' }
if ($Play) { & (Join-Path $BuildRoot 'unify_game.exe') }
