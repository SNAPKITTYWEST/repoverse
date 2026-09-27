# Repoverse / SNAPKITTY game

## Play the Windows build

The native game opens the SNAPKITTY menu. Enter or Space starts Bifrost Runner.
A/D or arrows move, Space/W jumps, E talks near the Architect, F5 saves, F9
restores, and Escape returns to the menu. I opens the about panel. R opens the
repository browser; Left/Right selects a repository and Escape returns.

Build from the repository root with PowerShell:

    ./tools/Build-Snapkitty.ps1 -Play

After building, run python tools/package-snapkitty.py to create
out/Snapkitty-Windows.zip. Extract it and double-click Play.cmd.
The package includes runtime assets, original artwork, notices and checksums.

Requires CMake, MinGW GCC/G++, and network access for the first SDL2 download.
The script pins SDL 2.32.10 and verifies its SHA-256. The runtime has no network
dependency. Generated sprites and audio remain part of the existing Unify build.

## What is shared

The title, repository browser, gameplay, physics, WASM compute, saves and artwork
are shared Unify Lua/assets. The SDL backend provides a desktop window. Unreal
provides a Canvas renderer, input and audio through the existing Unify backend.
This is a native 2D game hosted inside Unreal, not a new browser simulation.

The existing demo remains available with --scene title. The new default is
snapkitty, which opens snapkitty_level. The player uses the original kitten
cutout; motion uses the existing run/jump state machine and procedural squash,
not invented sprite-sheet frames. The Architect is an interactive NPC near the
start. Event pickups, obstacles, moving platforms, the finish trigger and
save/restore use the existing platformer systems.

## Repoverse data bridge

The C# core remains the authority for world generation. Export a snapshot and
manifest using the existing CLI, then generate the read-only repository scene:

    dotnet run --project src/Repoverse.Cli -- ingest-local . --owner SNAPKITTYWEST --out out/snapshot.json
    dotnet run --project src/Repoverse.Cli -- generate --snapshot out/snapshot.json --out out/manifest.json
    python tools/export-unify-world.py out/manifest.json

The exporter accepts schema 1 and encodes text as Lua string bytes, never as
executable source. It does not modify the manifest or run repository code.
The checked-in menu data is a snapshot of this repository, not live GitHub data.
The browser shows category, district, activity, construction, rooms and seed.
It is not yet the streamed 3D voxel district described in HANDOFF.md.

## Unreal project

See unreal/README.md. The project enables the UnifyUnreal plugin, assigns the
Unify GameMode, creates a host automatically, and stages ordinary files as
NonUFS assets because the engine opens them through its file loader.

## Artwork and provenance

Originals and the upstream AGPL license are under assets/snapkitty. Four QOI
runtime copies were converted losslessly, with pixel equality checked.
The source and runtime SHA-256 hashes are in runtime-manifest.json.
No original was recolored, resized or overwritten; scene tint is applied at
runtime for readability. Both original JPGs and transparent PNGs are retained.
The upstream source commit is recorded in source-manifest.json. This does not
replace the licensing terms of unrelated Repoverse code.

Regenerate texture files with python tools/import-snapkitty.py (Pillow required).
The checked-in QOI files make Pillow unnecessary for ordinary game builds.

## Validation

CTest runs the engine suite, the original demo playthrough and the SNAPKITTY
playthrough. The latter exercises the actual game rather than a mock scene.
An additional engine test covers the about panel, repository scene, play
transition, original textures and Architect interaction. Screenshots are
written under unify/build-game/snapkitty-evidence.

The Unreal Editor is not installed in the inspected locations on this machine.
Its C++ integration therefore still needs compilation and a PIE check with the
user's engine version. SDL/headless results do not establish Unreal compilation.
