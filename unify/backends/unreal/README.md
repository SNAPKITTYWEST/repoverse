For the ready-to-configure Repoverse host project and automated setup, see
[unreal/README.md](../../../unreal/README.md). The manual backend notes follow.

# UNIFY Unreal backend

An Unreal Engine 5 plugin that runs UNIFY games inside Unreal. It implements `unify::Backend`, the same
interface as the SDL2 and headless backends:

| UNIFY | Unreal |
|---|---|
| `RenderDevice::create_texture` | transient `UTexture2D` (nearest filtering) |
| `RenderDevice::draw(Batch)` | one `FCanvasTriangleItem` per UNIFY batch, drawn in `AUnifyHUD::DrawHUD` |
| `AudioDevice` | `UUnifySynthComponent::OnGenerateAudio` runs UNIFY's mixer on the audio render thread |
| `Backend::poll` | `AnyKey` press/release bindings, stamped with `FPlatformTime` into UNIFY's input buffer |
| `Backend::now_ns` | `FPlatformTime::Cycles64` |

Gameplay stays in UNIFY Lua scripts. Scripts never see an Unreal type, and the same `game/assets`
folder runs unchanged on SDL2, headless and Unreal.

## Status

This source is written against the UE5 API but **has not been compiled**: UNIFY's CI has no Unreal
toolchain. Expect small API fixes on first build, for example `UTexture2D` field access differs between
UE 5.x minor versions. Scissor rectangles are recorded but not yet applied as canvas clip rects.

## Use

1. Build UNIFY with CMake. Set `UNIFY_ROOT` / `UNIFY_BUILD` for `UnifyUnreal.Build.cs`.
2. Copy this folder to `<Project>/Plugins/UnifyUnreal` and copy `game/assets` to `<Project>/Unify/assets`.
3. Place an `AUnifyHost` actor in a level, and set the GameMode's HUD class to `AUnifyHUD`.
