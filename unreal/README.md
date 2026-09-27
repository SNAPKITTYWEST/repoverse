# Repoverse Unreal host

This project hosts the real Unify game, not a separate reimplementation.
It uses the existing Canvas/synth/input backend and the same Lua scenes/QOI
textures as the desktop build. No UnrealSharp dependency is needed.

## Build and launch on Windows

Install Unreal Engine 5.4 or newer and its supported Visual Studio 2022 C++
toolchain. From the repository root, supply your actual engine directory:

    ./unreal/Prepare-Unreal.ps1 -EngineRoot 'D:/Epic/UE_5.6' -Launch

The script first checks that UnrealEditor.exe exists. It then builds the Unify
static libraries with MSVC, /MD and UNIFY_NO_GLOBAL_NEW=ON; copies the canonical
plugin and game assets into the project; builds RepoverseEditor; creates the
initial map through the editor's Python API if it does not exist; and optionally
opens the game. It stops on failure. The example path is not an installed-engine
claim. Use the same MSVC toolset as your Unreal installation.

The script sets UNIFY_ROOT and UNIFY_BUILD in its current process so UBT resolves
the real source headers and Release libraries. Rerun it after engine/asset changes.
Do not edit the generated Plugins/UnifyUnreal copy; edit unify/backends/unreal.
The runtime files are staged as NonUFS; no content import step is needed.

The native GameMode creates one AUnifyHost and assigns AUnifyHUD. StartPlay gives
keyboard focus to the game. No actor placement or Blueprint scripting is required.
The shared game menu defaults to the snapkitty scene.

## Boundary

Unreal source has not been compiled in this environment: no Unreal Editor/UBT
installation was found. No packaged Unreal executable is claimed. First validation:
build with the target editor, open /Game/Maps/Repoverse, Play, enter the level,
move/jump/talk, save/restore, return to menu and resize the window. Check audio
and PIE teardown. The backend still records scissor rectangles without applying
Canvas clipping; the new frontend does not rely on scissor clipping.

The repository browser uses an offline C# WorldManifest projection. Streaming
voxel chunks through a sidecar, 3D district navigation and Unreal voxel collision
are separate work described in the original HANDOFF.md, not implemented by this
2D host.
