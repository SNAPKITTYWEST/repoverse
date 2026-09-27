# UNIFY

UNIFY is a 2D game engine runtime written in C++17, with Lua gameplay, a CUDA-like kernel compiler that
targets its own WASM runtime, and interchangeable platform backends. `game/` holds a small platformer
built on it, which proves the runtime end to end.

```
UNIFY                          where
├── renderer                   engine/render      RenderDevice, Renderer2D (sort + batch), camera, atlas, QOI/PNG, bitmap font
├── physics                    engine/physics     rigid bodies, SAP broadphase, SAT/clip manifolds, sequential impulses, CCD, islands
├── audio                      engine/audio       mixer, buses, voices, WAV decoder, streaming ring buffer, device thread
├── input                      engine/input       timestamped event queue → per-tick InputFrame, actions/axes, buffering
├── animation                  engine/animation   sprite clips, keyframe tracks, loop/once/ping-pong, crossfade blending
├── scene                      engine/runtime     scene stack with per-scene resource ownership, prefabs, transitions
├── ECS                        engine/ecs         generational entities, sparse-set component pools
├── Lua                        engine/script      Lua 5.4 VM: own allocator + budget, engine-scheduled GC, sandbox
├── memory / GC                engine/memory      Frame/Pool/Object/Asset/Buffer allocators, pool-backed STL allocator
├── assets                     engine/assets      registry, ref-counted handles, cache, loaders
├── serialization              engine/serialization  tagged, versioned binary sections
├── save states                engine/runtime     save_state / load_state / snapshot / restore / state_hash
├── networking-ready state     engine/net         rollback buffer: snapshots + input log, rewind & resimulate
├── WASM runtime               engine/wasm        binary decoder, interpreter, shared linear memory, buffers
├── kernel compiler            engine/kernel      CUDA-like source → AST → IR → constant folding → WASM
└── Unreal backend             backends/unreal    UE5 plugin implementing unify::Backend (not compiled here, see Status)
```

## Build and run

Requires CMake ≥ 3.16, a C++17 compiler, and optionally SDL2 (`libsdl2-dev`) for the windowed backend.

```bash
cmake -S . -B build -G Ninja
cmake --build build          # also generates the game's sprites, SFX and music (unify_assetgen)
ctest --test-dir build       # 58 unit/integration tests + the definition-of-done playtest
./build/unify_game           # SDL2 window: arrows/AD move, Space jump, F5 save, F9 load, Esc menu
./build/unify_game --headless --frames 600 --screenshot out.png
./build/unify_playtest game/assets out/   # scripted playthrough with verification + screenshots
```

## The engine loop

Every real-time frame runs these stages. The playtest prints this order from the engine's own trace.

```
INPUT → INPUT BUFFER → FIXED SIMULATION → PHYSICS → GAMEPLAY / LUA → ANIMATION → AUDIO → RENDER PREPARATION → RENDER → PRESENT
        └─────────────────── zero or more fixed 60 Hz ticks ──────────────────┘
```

There are five separate clocks:

| Clock | Where | Used for |
|---|---|---|
| Wall / input clock | `Backend::now_ns()` | timestamping OS events, deciding when ticks are due |
| Simulation | `SimClock` (integer ticks) | all gameplay, physics and animation. Seconds are derived from ticks, never accumulated |
| Render | `RenderClock` | interpolation factor between the last two sim states |
| Audio | `Mixer::frames_rendered()` | advanced by the audio device thread, independent of frame rate |
| Input | event timestamps | an event is consumed by the tick whose window contains it |

A tick is due when `origin + (tick+1)·1e9/rate ≤ now`. After `max_ticks_per_frame`, a stall drops time
rather than spiralling. Rendering interpolates transforms, so 30, 60 and 144 FPS all run the same
60 ticks per second (tested).

**Input buffering.** OS events are queued with timestamps. Each tick drains the events stamped before
its end and folds them into an `InputFrame`: `pressed`, `released`, `repeated`, `held`, axes and pointer.
A press and release that both land between two ticks still reads as *pressed* in the next tick; the
playtest checks this with a 3 ms tap. `input:buffered("jump", n)` gives gameplay a press window.

## Determinism, save states, networking

- One seeded RNG (xoshiro128**) is the only random source. Lua's `math.random` is rebound to it.
  Lua has no `os` or `io`, so no wall-clock reads.
- The build uses `-ffp-contract=off -fno-fast-math`, and every container that affects the simulation
  iterates in a deterministic order.
- **Save state** = tagged binary sections: `HEAD` (magic, version, engine, tick rate), `TIME` (tick,
  timers), `RNG`, `SCEN` (scene stack, owned assets by name, buffers, kernels), `ECS` (entity pool and
  component pools in dense order), `PHYS` (bodies, *and contact impulses for warm starting*), `ANIM`,
  `INPT`, `CAMR`, `COMP` (live WASM buffers and kernel sources), `LUA` (the `game` table plus each scene's
  `state`), and `AUDI` (voices by asset name and position). There are no raw pointers; handles and names
  only. Unknown sections are skippable, and newer versions are refused.
- `save_state()` / `load_state()` from Lua are applied at the tick boundary. `snapshot()` / `restore()`
  are the in-memory forms, and `save_to_file()` writes atomically.
- **Round trip:** restore reproduces the saved `state_hash()` exactly, in the same engine or a fresh one.
  Replaying the recorded input from a restored state reproduces the original continuation bit for bit.
- `engine/net/rollback.h` keeps periodic snapshots plus the input log. `correct(tick, frame)` rewinds and
  re-simulates. A test proves that a late-corrected timeline equals the true one.

## Lua API

Scenes are Lua files returning a table with `init`, `load`, `update(dt)`, `render`, `on_contact(a, b,
kind, nx, ny, impulse)`, `on_timer(name)`, `on_saved`, `on_loaded` and `unload`. UNIFY owns the lifecycle.

```lua
entity:set_position(x, y)        entity:set_velocity(x, y)     entity:set_animation("run", blend)
physics:apply_force(entity, x, y)  physics:raycast(x1, y1, x2, y2, mask, ignore)  physics:add_static_box(...)
audio:play("audio/jump")         audio:stream("music/theme")   audio:stop(id, fade)   audio:set_bus_volume("music", v)
input:is_pressed("jump")         input:is_held("left")         input:buffered("jump", ticks)   input:axis("move")
world:spawn("prefabs/player", x, y)   world:find(name)         camera:set(x, y, zoom)   ui:text(...)  -- ui only in render()
timer.after(sec, name)   scene.transition("level")   animation.define{...}   map.load("maps/level1")
buffer.create(n)  buffer.upload(b, t)  buffer.download(b)      kernel.compile(src)  kernel.load(k)  kernel.dispatch(k, grid, block, ...)
save_state([slot])  load_state([slot])   gc_step() gc_budget(kb) gc_pause() gc_resume() gc_collect()
```

Lua only ever holds handles: entities, buffers and kernels are userdata that wrap a generational handle,
checked on every call. A destroyed entity raises `stale entity handle` instead of touching freed memory.
Lua memory comes from its own allocator with a hard cap (the default is 64 MB). The collector is stopped,
and the engine runs one budgeted incremental step per tick, plus a full collection only on scene loads.

## Kernel compiler and WASM runtime

```c
__global__ void vadd(const float* A, const float* B, float* C, int n) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n) C[i] = A[i] + B[i];
}
```

**Source → AST.** A lexer and recursive-descent parser handle `int`/`float`, pointers, `if`/`for`/`while`,
`break`/`continue`, casts, `sqrtf`/`fabsf`/`fminf`/`fmaxf`/`min`/`max`, and `threadIdx`/`blockIdx`/`blockDim`/`gridDim`.

**AST → IR.** A typed, name-resolved, structured IR with explicit conversions, printed by `print_ir`.
Constant folding and dead-branch elimination run on it.

**IR → WASM.** Each module imports `env.memory` and exports the kernel plus `<name>__dispatch(grid, block, ...)`,
which loops over the whole grid inside WASM.

**Runtime.** It decodes and validates the binary, builds block side tables, and interprets i32/i64/f32/f64.
Out-of-bounds access, divide by zero, stack overflow and fuel exhaustion are *traps*, returned as errors.
All kernels share one linear memory, and buffers are ranges handed out by `BufferAllocator`.

The demo runs `vadd` on the title screen and integrates the sparkle particles with a second kernel.

## Backends

| Backend | Render | Audio | Input | Status |
|---|---|---|---|---|
| Headless | software device, offscreen | null device thread (real time) | programmatic / replay | built and tested |
| SDL2 (native) | software device → SDL texture, integer scaling | SDL audio callback | keyboard, mouse, game controller, touch | built; smoke-tested with SDL's dummy drivers |
| Unreal | `FCanvasTriangleItem` per batch, transient `UTexture2D` | `USynthComponent` | `AnyKey` bindings | **source only, not compiled** (no UE toolchain in CI) |

The renderer is a `RenderDevice` interface. The engine ships a software rasterizer as its reference
device, and a GPU device would plug in at the same interface.

## Verification

- `unify_tests`: 58 tests covering every physics edge case in the spec (tunneling, bullets, resting
  contacts, stacking, penetration, zero and infinite mass, tiny and huge bodies, coincident centres,
  parallel edges, corner, edge and multi contacts, triggers, sleep and wake, filtering, NaN containment,
  exact save/restore). They also cover input buffering, the mixer, streaming and loop points, the audio
  device thread, renderer batching, blending, scissor and rotation, animation modes and blending, the
  allocators, handles, RNG, the kernel pipeline, WASM traps, scene-transition leak checks, determinism
  across engines, fresh-engine restore, Lua sandboxing, the handle and memory caps, GC scheduling,
  **zero heap allocations per steady-state tick**, stage order, frame-rate independence, and rollback.
- `unify_playtest`: the definition of done, from BOOT to CONTINUE PLAYING, on the real game with
  scripted input. Each stage is verified from engine state (hashes, counts, clips, voices) and
  screenshots are written.
- The whole suite and the playtest are clean under AddressSanitizer and UndefinedBehaviorSanitizer.

## Not included / known limits

- The **WASM *platform* backend** (compiling UNIFY itself to WebAssembly for browsers) is not built. It
  needs Emscripten, which isn't available here. The engine core has no platform dependencies, so it is
  a backend-sized job. UNIFY's WASM *runtime* for kernels is fully built.
- **Unreal backend**: see the table above.
- Audio decodes PCM16 WAV only. Physics has no joints. Scissor rectangles are not applied on the Unreal
  backend. The software renderer is the only render device.
