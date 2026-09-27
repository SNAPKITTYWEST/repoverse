# Repoverse handoff

**From:** the cloud session that built the C# core (Master Directive, Part I)
**To:** whoever builds the engine side and the back end locally (Rider, Unreal, Swift, CAD)
**State at handoff:** `dotnet build -warnaserror` is clean and all 72 tests pass. The pipeline has run end to end on real
data (the 15 sub-projects of `SNAPKITTYWEST/BOBS-Many-Voxel-Worlds`). Nothing has run inside Unreal, Swift or CAD
tooling yet. The Parts II and III directive text has not been received.

---

## 1. What exists

Read `README.md` for the full table. In one line: **GitHub or local git → `CanonicalRepository` → classifier →
deterministic `WorldManifest` → procedural chunks (32³, 16-bit voxels) → greedy-meshed quads**, plus snapshot
diff events, a fixed-step clock, versioned saves and `repoverse://` URIs.

| Path | Responsibility |
|---|---|
| `src/Repoverse.Core/Model/` | Canonical GitHub facts; `Derived<T>` + `Provenance` for everything inferred |
| `src/Repoverse.Core/Ingestion/` | `GitHubIngest` (REST, ETag cache, rate limits), `LocalGitIngest`, `SnapshotStore` (offline merge) |
| `src/Repoverse.Core/Generation/` | `Seed`, `Classifier`, `Overrides`, `RoomGraph`, `ManifestBuilder`, `WorldManifest` |
| `src/Repoverse.Core/Voxels/` | `Voxel`/`Chunk`, `BuildingRasterizer`, `WorldGenerator`, `WorldEdits`, `ChunkStreamer`, `GreedyMesher` |
| `src/Repoverse.Core/Simulation/` | `WorldEvents` + `SnapshotDiff`, `SimulationClock` |
| `src/Repoverse.Core/Persistence/` | `SaveGame` (schema 2, v1 migration) |
| `src/Repoverse.Core/WorldConstants.cs` | Every tunable dimension; change numbers here only |
| `src/Repoverse.Cli/` | `ingest-local`, `ingest-github`, `generate`, `stats`, `export-mesh`, `diff` |
| `preview/index.html` | Three.js viewer for `export-mesh` output. A debugging aid, not the game |
| `tests/` | 72 behaviour tests; run `dotnet test` before and after every change to the core |

---

## 2. Contracts the back end must honour

The **C# types are the source of truth**. The JSON below is what they serialise to (`Json.Options`: camelCase,
enums as camelCase strings, nulls omitted). If you need a field, add it in C# and bump the schema version.
Don't patch the JSON.

### 2.1 World manifest (`generate` → `manifest.json`, schema 1)

```jsonc
{
  "schemaVersion": 1, "generator": "repoverse-gen/1", "worldSeed": "000000005aa4c177",
  "snapshots": { "snapkittywest/guppy": "git:<sha>" },          // repo key → snapshot id it was built from
  "buildings": [{
    "repo": "SNAPKITTYWEST/guppy",                              // display form; identity is the lower-cased key
    "category":     { "value": "compiler", "why": { "rule": "classifier:rules", "evidence": "mentions-compiler (score 5)" } },
    "district":     { "value": "kernel",   "why": { ... } },
    "archetype":    { "value": "foundry",  "why": { ... } },
    "material": "metal",
    "plot": { "district": "kernel", "slot": 0 },                // sticky across regenerations (pass --previous)
    "footprint": { "x": 257, "z": 254, "width": 14, "depth": 19 },   // voxel units, ground plane
    "floors": 1, "roomsPerSide": 2, "roomDepth": 6, "height": 7,
    "activity":     { "value": "low", "why": { ... } },           // dormant | low | moderate | high
    "construction": { "value": "unreleased", "why": { ... } },    // unreleased | prerelease | released | archived
    "landmark": false, "population": 1, "seed": "d17a3db530f2e546",
    "interior": {
      "rooms": [{ "id": 3, "name": "examples", "purpose": "workshop", "floor": 0, "directories": ["examples"],
                  "fileCount": 3, "artifacts": [{ "path": "examples/grover_2qubit.py", "kind": "sourceTerminal",
                  "sizeBytes": 4083, "language": "Python" }] }],
      "doors": [{ "a": 0, "b": 1 }]
    }
  }],
  "dependencies": [{ "from": "owner/a", "to": "owner/b" }]
}
```

**Rule:** Unreal actors are built *from* this and never written back into it (§5). The same snapshot, seed and
previous manifest must always produce byte-identical JSON; `GenerationTests` enforces this.

### 2.2 Voxels and chunks

- `Voxel` is a `ushort`: bits 0–7 `BlockType`, bits 8–11 `MaterialFamily`, bits 12–15 state (activity level for
  walls, floors and roofs). `MeshKey = packed & 0x0FFF`, and one material/draw group per key.
- A chunk is 32×32×32, indexed as `(y * 32 + z) * 32 + x`. Chunk Y layers run from 0 to `WorldGenerator.MaxChunkY`
  (currently 2).
- World units are **1 voxel = 1 unit**. Pick one Unreal scale and put it in one place. Suggested: 1 voxel = 50 cm,
  a 6-voxel storey = 3 m.
- Axes: **Y is up** in the core. Unreal is Z-up, so map core (x, y, z) → UE (x, z, y) and flip winding, or
  mirror one axis. Do it in exactly one conversion function.
- `GreedyMesher.Build(chunk, neighbourSampler)` → `Quad(axis, positive, layer, u, v, width, height, meshKey)`.
  `GreedyMesher.Corners()` returns counter-clockwise world-space corners (core handedness). Pass a
  neighbour sampler, or chunk-border faces will not be culled.
- Interactive things are **not voxels**. `BuildingRasterizer.Objects(building)` yields `WorldObject`
  (id, repo, x, y, z, kind, sourcePath, roomId, actions). Spawn these as actors or instances.
- `BuildingRasterizer.Plan(building)` gives each room's voxel rectangle and floor Y. Use it for cutaway,
  floor isolation and room highlighting.

### 2.3 Other formats

| Format | Where | Notes |
|---|---|---|
| Snapshot | `Ingestion/SnapshotStore.cs` | schema 1; last-known-good; written atomically |
| Save | `Persistence/SaveGame.cs` | schema 2; player + edits + tick only; add migrations to `Migrations` |
| Events | `Simulation/WorldEvents.cs` | typed records; `SnapshotDiff.Diff(before, after)` produces them |
| URI | `RepoverseUri.cs` | `repoverse://owner/repo/path#L12`, `?room=N`, `@x,y,z` |
| Mesh export | `Cli/Program.cs` `export-mesh` | **debug format only**: enum names here are PascalCase, unlike the manifest. Don't build the engine on it; use §3.1 |

---

## 3. What to build next, in order

Order follows the directive's vertical-slice rule (§55–57): get one district walkable end to end before scaling.

### P0: finish the Part I vertical slice

**3.1 C# ↔ Unreal bridge (decide first; everything else depends on it)**
The core is .NET 8 and Unreal is C++. The options, in my recommended order:

1. **Sidecar process (recommended to start).** Run the core as a local server. An Unreal C++ plugin talks to it
   over a named pipe or localhost TCP with length-prefixed binary messages. It's the easiest to debug
   (both sides test alone), the core stays engine-free, and the repo already uses this pattern
   (`voxel/server/QuantumVoxelServer.cs` in BOBS-Many-Voxel-Worlds). Minimum messages: `LoadManifest`,
   `SetFocus(x,z)` → stream `ChunkMesh`/`ChunkUnloaded`, `GetObjects(buildingKey)`, `ApplyEdit`, `Save`/`Load`,
   `Events` (push).
2. **Embed .NET in Unreal via `hostfxr`.** In-process with no IPC cost, but you own the hosting and threading.
3. **UnrealSharp or a similar plugin.** Quickest to write gameplay in C#, but it's a third-party dependency on
   your engine version.

*Done when:* Unreal shows the chunks around a focus point, updates as the focus moves, and memory stays flat
while travelling (mirror `VoxelTests.Streaming_memory_is_bounded_by_radius_while_travelling`).

**3.2 Unreal chunk rendering (C++)**
- Build one mesh section per `MeshKey` from `Quad`s. Use `RealtimeMeshComponent` or
  `UProceduralMeshComponent`, and do not spawn one actor per voxel (§14).
- Collision comes from the same quads (complex-as-simple is fine to start).
- Map `MaterialFamily` × `BlockType` to material instances. Windows are translucent.
- *Done when:* the 15-building district renders at target frame rate, and `stat unit` and draw calls are logged
  (§44).

**3.3 Player, interaction, camera modes**
- First/third-person walk with collision. Enter through the east-wall door (`Plan().Entrance`) and climb the
  ladder shaft between floors.
- Interaction comes from `WorldObject.Actions` (the object advertises its verbs; the player code doesn't
  hard-code them, §52).
- Camera modes World, Architectural Orbit (roof hide via `BlockType.Roof`, floor isolation via
  `Plan().Rooms[].FloorY`), Code and City Overview. Selection is one `repoverse://` URI shared by all modes (§73).
- *Done when:* you can walk the whole §57 success path: enter building → pick room → pick file → Code
  Mode → back outside → travel → quit → reload → same state.

**3.4 Source inspection**
- Fetch file contents on demand: from the GitHub contents API, via `GitHubIngest`'s cache pattern, or from a
  local clone. Page large files (§38). Nothing is fetched at manifest time.

**3.5 Save/load wiring**
- On quit, `SaveGame.Capture(manifest, player, edits, clock.Tick)`; on boot, `Read` then `RestoreEdits`. Player
  position and mode must come back exactly.

**3.6 Dependency extraction** (core, C#)
- Fill `CanonicalRepository.DependsOn` by parsing `Cargo.toml`, `package.json`, `*.csproj`, `go.mod`,
  `pyproject.toml`/`requirements.txt` and `.gitmodules`, matched against repos in the world. Edges then flow
  into the manifest automatically, with a test for each format.

### P1: control plane and navigation

- **Swift control plane (§33–34).** An `AppState` holding selection, camera, overlays, simulation state and
  graph filters, bridged to the engine. It controls intent only and must never copy world data. Every toggle
  in §34 must change real filter state (add a test per toggle).
- **CSS design tokens (§35).** Spacing, radii, blur, type scale, status colours, focus, hover, selected,
  disabled and stale states. Pair each colour with a shape or icon (§70). Use it for any HTML/UMG-web overlays.
- **Graph Mode (§21).** Draw `dependencies` edges between buildings, filterable by category and depth.
- **Search, command palette, world map, bookmarks, fast travel (§48–50, §74–75).** The data is already there:
  manifest buildings plus artifacts plus `RepoverseUri`.
- **Live sync loop.** Periodic `ingest-github`, then `SnapshotDiff`, then events drive visuals (§26, §92):
  release → illumination, archived → archive state, new files → rebuild that building's chunks only.

### P2: later

- CAD import pipeline, offline only (§24): DWG/DXF/IFC → cleaned mesh + metadata → packaged asset,
  referenced from `Overrides` (`landmark`, custom archetype).
- Audio, accessibility pass, and diagnostics overlay (§61, §69–70).
- Parts II and III (agents, simulation, 388-repo scale, packaging), once the directive text is available.

---

## 4. Known gaps in the core (fix before scaling to 388 repos)

| Gap | Where | Impact |
|---|---|---|
| **District overflow throws.** Each district holds 144 plots, and a 145th repo in one district fails the whole build | `ManifestBuilder.Build` | Likely to hit at 388 repos if the classifier skews. Needs §85 expansion (a new ring or an overflow district) |
| `DependsOn` never populated | ingesters | Graph Mode has no data (see 3.6) |
| Contributors capped at 100 (1 page); recent commits capped at 300 (3 pages) | `GitHubIngest` | Fine for activity levels; wrong as absolute counts |
| `.v` counts as both Coq and Verilog | `Classifier` | Ties break by enum order; provenance shows it. Use overrides |
| One floor plan (a corridor spine) for every archetype | `BuildingRasterizer` | Buildings read alike; archetype shells are future work |
| Headers are added on every `GitHubIngest` construction | `GitHubIngest` ctor | Duplicate headers if one `HttpClient` is reused. Give each ingest its own client, or move the headers onto the request |
| Local ingest treats git tags as releases, whole-repo only | `LocalGitIngest` | Sub-project buildings always show `unreleased` (scaffolding) |
| Classifier is keyword rules | `Classifier` | Fine for a slice. An AI classifier must follow §82 (cached, versioned, overridable, never blocking) |

---

## 5. Setup

```bash
# .NET 8 SDK + git
dotnet build -warnaserror && dotnet test
dotnet run --project src/Repoverse.Cli -- ingest-local <any-git-checkout> --owner SNAPKITTYWEST --subprojects --out out/snapshot.json
dotnet run --project src/Repoverse.Cli -- generate --snapshot out/snapshot.json --out out/manifest.json
dotnet run --project src/Repoverse.Cli -- stats --manifest out/manifest.json
GITHUB_TOKEN=<fine-grained, read-only> dotnet run --project src/Repoverse.Cli -- ingest-github SNAPKITTYWEST --out out/snapshot.json
```

Keep the token in the environment or the OS keychain, never in files (§41). `GitHubIngest.Redact` scrubs
tokens from error text.

## 6. Decisions you need to make

1. **Bridge:** sidecar, hostfxr or UnrealSharp (3.1). I recommend the sidecar.
2. **Unreal version and mesh component:** RealtimeMesh plugin or built-in ProceduralMesh.
3. **Voxel scale in Unreal:** 50 cm is suggested.
4. **Swift's target platform:** macOS only, or an iPad companion over the network. This decides whether Swift
   talks to the engine in-process or over the same sidecar protocol.
5. **Scope of the first live ingest:** all 349 public repos plus private ones, or a curated list via
   `ingest-github --limit` / overrides.
