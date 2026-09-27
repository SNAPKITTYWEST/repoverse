# SNAPKITTY: Bifrost Runner — 2D game

**Play it live:** https://ahmadaliparr.github.io/snapkitty-2d/

Built from your four art pieces: the hoodie kitten (player), the suited
Architect (NPC), the SNAP-OS dashboard (HUD language), and the
Snapkitty civilization poster (world backdrop).

## Play it now — `index.html`

A playable 2D platformer prototype of the full game design. Open in any
browser, no build step.

- **Move:** A/D or arrows · **Jump:** Space/W (coyote time included)
- **E:** talk to the Architect at checkpoints · **R:** restart after winning
- Seal all **8 Bifrost event cubes**, then reach the **Bifrost terminal**
- Gaps respawn you; HUD tracks sealed events + ledger integrity

Verified: `node --check` clean, headless DOM-stubbed run of the full
game loop (boot, run, jump, all 8 pickups, NPC dialogue open/close,
win banner, reset) — all OK. Layout composited against the real art
and visually inspected.

## Build it for real — `UnrealProject/` (C# / UnrealSharp)

Unreal Engine 5.5+ project using **C# via UnrealSharp** + Paper2D:

- `Snapkitty2D.uproject` — module `Snapkitty2D`, depends on
  `Paper2D`, `EnhancedInput`, `UnrealSharpCore`
- `Source/Snapkitty2D/`:
  - `SnapkittyPlayer.cs` — plane-locked 2D character controller,
    run/jump, flipbook animation states (idle/run/jump)
  - `ArchitectNPC.cs` — dialogue NPC, talk radius, briefing lines
  - `BifrostPickup.cs` — bobbing/spinning collectible event cube
  - `SnapkittyGameMode.cs` — sealed-event counter, win condition,
    Blueprint events for UI/fanfare
  - `SnapkittyHUD.cs` — SNAP-OS styled HUD bindings (event counter,
    integrity bar, timed dialogue box)
- `Config/DefaultEngine.ini` — 2D-oriented defaults

Open the `.uproject` in UE5, import `art/` as Paper2D sprites, subclass
the C# classes in Blueprints, build `L_BifrostRunner`.

## Art — `art/`

| file | role |
|---|---|
| `kitten.png` | player sprite (background-removed cutout) |
| `suited-man.png` | Architect NPC sprite (background-removed cutout) |
| `civilization.jpg` | parallax world backdrop |
| `snap-os.jpg` | HUD/title-screen reference |
| `*.jpg` originals | kept as fallback sources |

(Cutouts produced with `rembg`; the web prototype auto-falls back to
the `.jpg` originals if a `.png` is missing.)
