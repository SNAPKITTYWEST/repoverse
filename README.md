# Repoverse: engine-independent core

This is the C# domain core of **SNAPKITTY REPOVERSE** (Master Directive, Part I). It turns GitHub
repositories into a deterministic world manifest and a chunked voxel world. Open `Repoverse.sln` in Rider.

```
GitHub / local git ──► CanonicalRepository ──► Classifier ──► WorldManifest ──► WorldGenerator ──► Chunk ──► GreedyMesher
   (Ingestion/)          (Model/)            (Generation/)   (Generation/)      (Voxels/)                   (Voxels/)
```

Nothing in `Repoverse.Core` depends on Unreal. The engine consumes the manifest and chunk data. It
never becomes the source of truth (§5).

## What is built and tested

| Directive § | What exists | Where |
|---|---|---|
| 6 Canonical model | GitHub facts, kept separate from derived values; every derived value carries `Provenance` | `Model/` |
| 25–27 Ingestion | GitHub REST client: `Link` pagination, ETag conditional requests with a disk cache, rate-limit detection, failures isolated per repo. Local-git ingester for offline use and monorepos | `Ingestion/GitHubIngest.cs`, `LocalGitIngest.cs` |
| 27 Offline | Merges snapshots per repo. An empty or rate-limited refresh never replaces good data; unreached repos are kept and marked stale | `Ingestion/SnapshotStore.cs` |
| 8 Seeds | FNV-1a and SplitMix64, stable across processes and platforms (`string.GetHashCode` is not) | `Generation/Seed.cs` |
| 9–10, 80, 82–83 | Rule-based classifier with whole-word keyword scoring (purpose outweighs language), districts, archetypes, JSON overrides, landmarks | `Generation/Classifier.cs`, `Overrides.cs` |
| 7, 85 Manifest | Deterministic manifest. Placement is sticky: existing buildings keep their plot when repos are added | `Generation/ManifestBuilder.cs` |
| 78–79, 87–89 Interiors | Room graph built from the directory tree, with ignore rules (`node_modules`, `target`…) and semantic compression down to a room budget. Reachability is validated | `Generation/RoomGraph.cs` |
| 11–12 Voxels | 16-bit packed voxels, 32³ chunks (64 KiB each). Interactive objects are entities, not voxels | `Voxels/Voxel.cs`, `BuildingRasterizer.cs` |
| 11, 13 Streaming | Nearest-first async generation on the thread pool, bounded in-flight work, hysteresis unloading, cancellation. Player edits are stored apart from procedural state | `Voxels/ChunkStreamer.cs`, `WorldGenerator.cs` |
| 14 Meshing | Greedy mesher with cross-chunk face culling and per-material draw groups | `Voxels/GreedyMesher.cs` |
| 26, 32 Events | Typed events computed by diffing two snapshots (added, archived, release, activity, structure…) | `Simulation/WorldEvents.cs` |
| 31 Clock | Fixed-step simulation clock, decoupled from frame rate: speed, pause, scheduling, spiral-of-death guard | `Simulation/SimulationClock.cs` |
| 42–43 Saves | Versioned save of player state and edits, with a v1→v2 migration. Saves from a newer build are refused | `Persistence/SaveGame.cs` |
| 76–77 URIs | `repoverse://owner/repo/path#L12`, `?room=`, `@x,y,z`; maps back to GitHub URLs | `RepoverseUri.cs` |

There are 72 xUnit tests (`tests/`). They check behaviour, not constructors: determinism, sticky placement, no
overlapping plots, room reachability found by flood-filling walkable air in the rasterized geometry, greedy
quads covering exactly the visible faces, streaming memory bounded while travelling, edits surviving
unload/reload, ETag cache hits, rate limits, partial failures, offline fallback, token redaction, save
migration, and URI round-trips.

## Run it

Requires the .NET 8 SDK and git.

```bash
dotnet test
# Offline: any git checkout works; --subprojects treats each top-level folder as a repo.
git clone https://github.com/SNAPKITTYWEST/BOBS-Many-Voxel-Worlds.git ../BOBS-Many-Voxel-Worlds
dotnet run --project src/Repoverse.Cli -- ingest-local ../BOBS-Many-Voxel-Worlds --owner SNAPKITTYWEST --subprojects --out out/snapshot.json
dotnet run --project src/Repoverse.Cli -- generate --snapshot out/snapshot.json --out out/manifest.json
dotnet run --project src/Repoverse.Cli -- stats --manifest out/manifest.json
dotnet run --project src/Repoverse.Cli -- export-mesh --manifest out/manifest.json --out preview/world-mesh.json
# then serve preview/ (e.g. `python3 -m http.server -d preview`) and open index.html?focus=snapkittywest/quantum-world

# Live GitHub (token read from the environment, never stored):
GITHUB_TOKEN=... dotnet run --project src/Repoverse.Cli -- ingest-github SNAPKITTYWEST --out out/snapshot.json
```

On the 15 sub-projects of `BOBS-Many-Voxel-Worlds`: 15 buildings, about 60 chunks generated in 30–40 ms,
and about 170k visible faces meshed into about 7.2k quads (about 4% of naive).

`preview/index.html` is a Three.js developer view of the exported mesh, used to check the generator by
eye. It is not the game renderer.

## Unified engine: compute kernels to WebAssembly

`unified-engine/` is a JavaScript compiler that takes CUDA-style `__global__` kernels and turns them into
WebAssembly, plus a host runtime that runs them. It needs Node 22+ and has no native dependencies.

| Stage | Files |
|---|---|
| Lexer, parser → AST | `kernel/compiler/lexer.js`, `parser.js`, `kernel/ast/ast.js` |
| AST → typed, structured IR (C scoping and conversions, short-circuit `&&`/`\|\|`) | `kernel/ir/lowering.js`, `kernel/ir/ir.js` |
| IR → wasm binary (no external toolchain) | `wasm/modules/codegen.js`, `encoder.js`, `wasm/abi/abi.js` |
| Shared memory, typed device buffers, `launch({grid, block})` | `wasm/runtime/runtime.js` |

```bash
cd unified-engine && npm ci && npm test   # 55 tests, including a particle-physics step
```

```js
const runtime = new KernelRuntime();
const add = await runtime.load(`__global__ void add(const float* A, const float* B, float* C, int N) {
  int i = blockIdx.x * blockDim.x + threadIdx.x; if (i < N) C[i] = A[i] + B[i]; }`);
const C = runtime.alloc('f32', n);
add.launch({ grid: Math.ceil(n / 256), block: 256 }, runtime.upload(a), runtime.upload(b), C, n);
C.read();
```

Limits: x dimension only, one kernel per module, no `__device__` functions, and no per-buffer bounds checks.
The `test:lua`, `build:kernel` and `demo` scripts point at layers that are not written yet.

## UNIFY: 2D engine runtime and demo game

`unify/` is a C++17 2D game engine (renderer, physics, audio, input, animation, ECS, Lua 5.4 gameplay, save
states, rollback buffer), with its own CUDA-like kernel compiler and WASM interpreter, and a small platformer in
`unify/game/` that exercises all of it. It was merged here with its history kept (`git subtree`). See
[`unify/README.md`](unify/README.md).

```bash
cd unify
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure   # 58 tests + the definition-of-done playtest
./build/unify_game                           # SDL2 window if libsdl2-dev is installed, else --headless
```

This builds on Linux (GCC or Clang) and on Windows with MinGW-w64 GCC. MSVC is not supported. The Unreal backend in
`unify/backends/unreal` has never been compiled.

`unify/engine/kernel` + `unify/engine/wasm` and `unified-engine/` are two independent implementations of the
same kernel → WASM idea (C++ with its own interpreter, and JavaScript running on the host's WebAssembly). They don't
share code yet.

## Not built yet

These need tools this repository's CI and cloud sessions don't have. They are left out rather than stubbed (§2):

- **Unreal / C++**: the engine project, the chunk-mesh upload (`ChunkMesh.Quads` maps directly onto
  `UProceduralMeshComponent` or a RealtimeMesh), and Lumen/Nanite materials.
- **Swift control plane** (§33–34) and the **CSS token layer** (§35) for in-engine UI.
- **CAD import pipeline** (§24).
- **Dependency extraction**: the `DependsOn` edges are modelled and rendered into the manifest, but not yet
  parsed from `Cargo.toml`, `package.json` and similar files.
- Parts II and III of the directive (agents, simulation, 388-repo scale, packaging).

## Known limits

- Classification is keyword and file-extension rules. `.v` counts as both Coq and Verilog. Ties
  break by category order, and the provenance string shows when that happened. Use `overrides.json` for intent.
- One floor plan (a corridor spine) for every archetype. Archetype-specific shells are future work.
- The local ingester's "releases" are git tags, and only for whole-repo ingests.

## SNAPKITTY game and Unreal host

[Play/build guide](docs/SNAPKITTY-GAME.md) · [Unreal setup](unreal/README.md)

The Unify launcher now opens an asset-backed SNAPKITTY menu, a playable Bifrost Runner level, and a read-only repository browser exported from the C# manifest. The same Lua game and artwork are wired into an Unreal host project. See the guides for verified native behavior and the Unreal compilation boundary.

## 3D world bridge (engine validation pending)

The C# sidecar and native Unreal district client are implemented in
`src/Repoverse.Sidecar`, `src/Repoverse.Core/Bridge`, and the existing Unreal
module. [Build instructions and exact validation boundary](docs/REPOVERSE-3D.md).
The service is tested; the Unreal client still requires UBT compilation and a
real editor playthrough. The older handoff task list remains historical context.
