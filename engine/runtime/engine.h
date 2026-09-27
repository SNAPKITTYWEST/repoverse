#pragma once
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "backend.h"
#include "../core/core.h"
#include "../ecs/ecs.h"
#include "../physics/physics.h"
#include "../animation/animation.h"
#include "../assets/assets.h"
#include "../script/lua_vm.h"
#include "../wasm/compute.h"
#include "../memory/memory.h"

namespace unify {

/// Scheduler stages, in execution order. Recorded per tick/frame for tracing and tests.
enum class Stage : uint8_t { Input, InputBuffer, FixedSimulation, Physics, Gameplay, Animation, Audio, RenderPrep, Render, Present, Count };
const char* stage_name(Stage s);

struct PrefabDef {
    std::string name;
    bool has_sprite = false;
    std::string texture, region;
    Vec2 sprite_size{1, 1};
    int16_t layer = 0;
    float depth = 0;
    Color tint;
    bool has_body = false;
    BodyDef body;
    std::string animation;   // initial clip
    uint32_t tags = 0;
};

struct Timer { uint64_t fire_tick; uint64_t interval_ticks; std::string name; uint16_t scene; };

/// A loaded scene: its script table and every resource it owns. Unloading a scene frees
/// all of it, which is what makes transitions leak-free.
struct Scene {
    uint16_t id = 0;
    std::string name;
    int lua_ref = -1;                 // registry ref to the table returned by the scene script
    AssetHandle script;
    std::vector<AssetHandle> assets;
    std::vector<AudioHandle> voices;
    std::vector<BufferHandle> buffers;
    std::vector<KernelHandle> kernels;
};

struct ResourceCounts {
    uint32_t entities, bodies, anim_players, voices, buffers, kernels, assets, textures, lua_refs;
    bool operator==(const ResourceCounts& o) const {
        return entities == o.entities && bodies == o.bodies && anim_players == o.anim_players && voices == o.voices && buffers == o.buffers &&
               kernels == o.kernels && assets == o.assets && textures == o.textures && lua_refs == o.lua_refs;
    }
};

struct FrameStats {
    uint32_t ticks_this_frame = 0;
    double stage_ms[size_t(Stage::Count)] = {};
    uint64_t heap_allocs_last_tick = 0;
    size_t lua_bytes = 0;
};

class Engine {
public:
    static constexpr uint32_t kSaveVersion = 1;
    static constexpr const char* kEngineVersion = "UNIFY 1.0";

    Engine(Backend& backend, EngineConfig config);
    ~Engine();

    /// BOOT → LOAD ASSETS → CREATE WORLD: starts devices, Lua, and the boot scene.
    bool boot(std::string& error);
    /// One real-time frame: poll input, run due fixed ticks, render, present.
    void frame();
    /// One fixed simulation tick (INPUT BUFFER → FIXED SIM → PHYSICS → GAMEPLAY → ANIMATION → AUDIO).
    void tick() { tick_impl(nullptr); }
    /// Runs one tick using a recorded/networked InputFrame instead of live input. This is
    /// the replay / rollback path: same state + same frames = same result.
    void replay_tick(const InputFrame& frame) { tick_impl(&frame); }
    /// RENDER PREP → RENDER → PRESENT with interpolation factor alpha.
    void render(float alpha);
    /// Runs until the backend or gameplay requests quit.
    int run();
    bool quit_requested() const { return quit_; }
    void request_quit() { quit_ = true; }

    // ---- save states --------------------------------------------------------------------
    std::vector<uint8_t> save_state(bool include_audio = true);
    bool load_state(const std::vector<uint8_t>& bytes, std::string& error);
    std::vector<uint8_t> snapshot() { return save_state(); }
    bool restore(const std::vector<uint8_t>& s, std::string& e) { return load_state(s, e); }
    bool save_to_file(const std::string& slot, std::string& error);
    bool load_from_file(const std::string& slot, std::string& error);
    /// Hash of all deterministic simulation state (excludes audio playback, which runs on
    /// its own clock). Equal hashes = equal simulations: the lockstep/rollback check.
    uint64_t state_hash();

    // ---- scenes / entities ----------------------------------------------------------------
    bool transition(const std::string& scene, std::string& error);
    bool push_scene(const std::string& scene, std::string& error);
    bool pop_scene(std::string& error);
    const Scene* active_scene() const { return scenes_.empty() ? nullptr : &scenes_.back(); }
    EntityHandle instantiate(const std::string& prefab, Vec2 position, bool persistent, std::string& error);
    bool destroy_entity(EntityHandle e);
    ResourceCounts resources() const;

    // ---- input injection (headless / replay) ----------------------------------------------
    /// Queues a button event that lands inside the next simulation tick's input window.
    void inject(uint16_t code, bool down);
    uint64_t next_tick_input_ns() const;

    // ---- subsystems -------------------------------------------------------------------------
    World& world() { return world_; }
    PhysicsWorld& physics() { return physics_; }
    AnimationSystem& animation() { return anim_; }
    InputSystem& input() { return input_; }
    Mixer& mixer() { return mixer_; }
    AssetManager& assets() { return *assets_; }
    ComputeRuntime& compute() { return compute_; }
    LuaVM& lua() { return *lua_; }
    Renderer2D& renderer() { return *renderer_; }
    Camera& camera() { return camera_; }
    Rng& rng() { return rng_; }
    const SimClock& sim_clock() const { return sim_; }
    const RenderClock& render_clock() const { return render_clock_; }
    RenderTarget& target() { return target_; }
    Backend& backend() { return backend_; }
    const EngineConfig& config() const { return config_; }
    const FrameStats& stats() const { return stats_; }
    const std::vector<Stage>& trace() const { return trace_; }
    const std::vector<std::string>& log_lines() const { return script_log_; }
    const PrefabDef* prefab(const std::string& name, std::string& error);

    // Used by bindings.
    bool in_render_phase() const { return rendering_; }
    void add_timer(double seconds, bool repeat, const std::string& name);
    void request_transition(const std::string& scene) { pending_transition_ = scene; }
    void request_save(const std::string& slot) { pending_ops_.push_back({true, slot}); }
    void request_load(const std::string& slot) { pending_ops_.push_back({false, slot}); }
    std::vector<uint8_t>& quick_slot() { return quick_slot_; }
    void script_log(const std::string& s);
    Scene* current_scene() { return scenes_.empty() ? nullptr : &scenes_.back(); }
    const BitmapFont& font() const { return font_; }
    const TextureAsset* texture(const std::string& name);
    void report_script_error(const std::string& where);

private:
    void tick_impl(const InputFrame* replay);
    bool load_scene_script(Scene& s, std::string& error);
    void unload_scene(Scene& s);
    bool call_scene(const char* fn, int nargs = 0);
    void sync_transforms_from_physics();
    void dispatch_physics_events();
    void fire_timers();
    void process_pending_state_ops();
    void mark(Stage s) { if (trace_.size() >= 64) trace_.clear(); trace_.push_back(s); }

    Backend& backend_;
    EngineConfig config_;
    World world_;
    PhysicsWorld physics_;
    AnimationSystem anim_;
    InputSystem input_;
    Mixer mixer_;
    AudioStreamer streamer_;
    std::unique_ptr<AssetManager> assets_;
    ComputeRuntime compute_;
    std::unique_ptr<LuaVM> lua_;
    std::unique_ptr<Renderer2D> renderer_;
    RenderTarget target_;
    Camera camera_;
    BitmapFont font_;
    Rng rng_;
    SimClock sim_;
    RenderClock render_clock_;
    FrameAllocator frame_alloc_{1u << 20};
    uint64_t sim_origin_ns_ = 0;     // input-clock time at which tick 0 began
    std::vector<Scene> scenes_;
    uint16_t next_scene_id_ = 1;
    std::vector<Timer> timers_;
    std::map<std::string, PrefabDef> prefabs_;
    std::map<std::string, AssetHandle> texture_cache_;
    std::vector<ContactEvent> events_scratch_;
    std::vector<Stage> trace_;
    FrameStats stats_;
    std::vector<std::string> script_log_;
    std::vector<uint8_t> quick_slot_;
    std::vector<std::pair<bool, std::string>> pending_ops_;  // (is_save, slot); "" = quick slot
    bool quit_ = false, booted_ = false, rendering_ = false;
    std::string pending_transition_;
};

/// Registers the UNIFY Lua API (entity, world, physics, audio, input, camera, ui, timer,
/// scene, buffer, kernel, gc, save_state/load_state) into the engine's Lua state.
void register_bindings(Engine& engine);

}  // namespace unify
