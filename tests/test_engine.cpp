// Engine-level tests against the real demo game content.
#include "test_main.h"
#include "../engine/runtime/engine.h"
#include <lua.h>
#include <memory>

using namespace unify;

namespace {
struct Harness {
    HeadlessBackend backend;
    std::unique_ptr<Engine> engine;
    explicit Harness(uint64_t seed = 7) {
        set_log_level(LogLevel::Error);
        EngineConfig c;
        c.asset_root = UNIFY_GAME_ASSETS;
        c.seed = seed;
        engine = std::make_unique<Engine>(backend, c);
        std::string err;
        booted = engine->boot(err);
        if (!booted) std::printf("    boot error: %s\n", err.c_str());
    }
    bool booted = false;
    void frames(int n) { for (int i = 0; i < n; i++) { backend.advance_clock(16666667); engine->frame(); } }
    void tap(uint16_t k) { engine->inject(k, true); frames(1); engine->inject(k, false); frames(1); }
    double num(const std::string& expr) {
        lua_State* L = engine->lua().L();
        if (!engine->lua().run("return " + expr, "probe", 1)) return -1e9;
        double v = lua_tonumber(L, -1);
        lua_pop(L, 1);
        return v;
    }
    bool lua_ok(const std::string& code) { return engine->lua().run(code, "probe"); }
    // Plays a fixed scripted sequence (same for every harness).
    void script(int ticks) {
        for (int t = 0; t < ticks; t++) {
            if (t % 50 == 0) engine->inject(key::Right, true);
            if (t % 50 == 30) engine->inject(key::Right, false);
            if (t % 37 == 5) engine->inject(key::Space, true);
            if (t % 37 == 15) engine->inject(key::Space, false);
            frames(1);
        }
    }
};
}  // namespace

TEST(engine_scene_transitions_do_not_leak) {
    Harness h;
    CHECK(h.booted);
    h.frames(3);
    ResourceCounts title = h.engine->resources();
    size_t asset_bytes = h.engine->assets().allocator().used();
    for (int round = 0; round < 3; round++) {
        h.tap(key::Enter);  // title -> level: ~360 entities, bodies, buffers, kernel, voices
        h.frames(30);
        CHECK(h.engine->active_scene()->name == "level");
        CHECK(h.engine->world().alive() > 300);
        CHECK(h.engine->compute().live_buffers() == 5 + 3 + 0 || h.engine->compute().live_buffers() >= 5);
        h.tap(key::Escape);  // level -> title
        h.frames(3);
        ResourceCounts back = h.engine->resources();
        CHECK(back == title);  // entities, bodies, anim players, voices, buffers, kernels, assets, textures, Lua refs
        CHECK(h.engine->assets().allocator().used() == asset_bytes);
    }
}

TEST(engine_same_seed_and_input_gives_same_simulation) {
    Harness a(99), b(99);
    a.tap(key::Enter); b.tap(key::Enter);
    a.script(300); b.script(300);
    CHECK(a.engine->state_hash() == b.engine->state_hash());
    CHECK(a.num("scene.state().jumps") == b.num("scene.state().jumps"));
    CHECK(a.num("scene.state().jumps") > 2);
}

TEST(engine_save_file_restores_in_a_fresh_engine) {
    std::vector<uint8_t> saved;
    uint64_t hash_at_save, hash_later;
    {
        Harness a;
        a.tap(key::Enter);
        a.script(120);
        saved = a.engine->save_state();
        hash_at_save = a.engine->state_hash();
        a.script(90);
        hash_later = a.engine->state_hash();
    }
    Harness b;  // a new engine, as after a process restart: boots into the title scene
    std::string err;
    CHECK(b.engine->load_state(saved, err));
    if (!err.empty()) std::printf("    load error: %s\n", err.c_str());
    CHECK(b.engine->active_scene()->name == "level");
    CHECK(b.engine->state_hash() == hash_at_save);
    b.script(90);  // the same scripted input from the same state...
    // ...reaches the same state, except for script() tick alignment which depends only on its own counter.
    CHECK(b.engine->state_hash() == hash_later);
}

TEST(engine_rejects_corrupt_or_foreign_save_states) {
    Harness h;
    std::string err;
    CHECK(!h.engine->load_state({1, 2, 3}, err));
    std::vector<uint8_t> s = h.engine->save_state();
    s.resize(s.size() / 2);
    CHECK(!h.engine->load_state(s, err) && !err.empty());
}

TEST(engine_lua_handles_are_validated) {
    Harness h;
    h.tap(key::Enter);
    h.frames(2);
    CHECK(h.lua_ok("local c = world:find('coin'); c:destroy(); _G.stale = c"));
    CHECK(!h.lua_ok("stale:position()"));
    CHECK(h.engine->lua().last_error().find("stale entity handle") != std::string::npos);
    CHECK(h.num("stale:valid() and 1 or 0") == 0);
    CHECK(!h.lua_ok("ui:text('x', 0, 0)"));  // only valid during render()
    CHECK(h.num("(os == nil and io == nil) and 1 or 0") == 1);  // sandboxed: no wall clock, no file access
}

TEST(engine_lua_gc_is_engine_scheduled_and_budgeted) {
    Harness h;
    CHECK(h.lua_ok("gc_pause(); junk = {}; for i = 1, 20000 do junk[i] = {i} end; junk = nil"));
    size_t before = h.engine->lua().bytes();
    h.frames(5);  // paused: ticks do not collect
    CHECK(h.engine->lua().bytes() >= before);
    CHECK(h.lua_ok("gc_resume(); gc_budget(16)"));
    h.frames(200);  // bounded incremental steps each tick reclaim it
    CHECK(h.engine->lua().bytes() < before);
    CHECK(h.lua_ok("gc_collect()"));
    CHECK(h.engine->lua().bytes() < before / 2);
}

TEST(engine_lua_memory_is_capped_separately) {
    HeadlessBackend backend;
    EngineConfig c;
    c.asset_root = UNIFY_GAME_ASSETS;
    c.lua_memory_limit = 4u << 20;
    Engine e(backend, c);
    std::string err;
    CHECK(e.boot(err));
    CHECK(!e.lua().run("local t = {} for i = 1, 1e7 do t[i] = i end", "hog"));
    CHECK(e.lua().last_error().find("not enough memory") != std::string::npos);
    CHECK(e.lua().bytes() <= e.lua().limit());
    CHECK(e.lua().run("x = 1 + 1", "after"));  // the VM survives the failed allocation
}

TEST(engine_steady_state_tick_does_not_heap_allocate) {
    Harness h;
    h.tap(key::Enter);
    h.frames(120);  // warm up: contacts, pools and scratch vectors reach steady size
    uint64_t worst = 0;
    for (int i = 0; i < 60; i++) { h.frames(1); worst = std::max<uint64_t>(worst, h.engine->stats().heap_allocs_last_tick); }
    CHECK(worst == 0);
    if (worst) std::printf("    worst tick: %llu allocations\n", static_cast<unsigned long long>(worst));
}

TEST(engine_scheduler_runs_stages_in_order) {
    Harness h;
    h.frames(2);
    const auto& t = h.engine->trace();
    std::vector<Stage> expect = {Stage::Input, Stage::InputBuffer, Stage::FixedSimulation, Stage::Physics, Stage::Gameplay,
                                 Stage::Animation, Stage::Audio, Stage::RenderPrep, Stage::Render, Stage::Present};
    CHECK(t == expect);
}

TEST(engine_fixed_timestep_is_independent_of_frame_rate) {
    Harness h;
    uint64_t t0 = h.engine->sim_clock().tick;
    for (int i = 0; i < 30; i++) { h.backend.advance_clock(1000000000ull / 30); h.engine->frame(); }   // 30 FPS for 1 s
    uint64_t at30 = h.engine->sim_clock().tick - t0;
    uint64_t t1 = h.engine->sim_clock().tick;
    for (int i = 0; i < 144; i++) { h.backend.advance_clock(1000000000ull / 144); h.engine->frame(); } // 144 FPS for 1 s
    uint64_t at144 = h.engine->sim_clock().tick - t1;
    CHECK(at30 >= 59 && at30 <= 61);
    CHECK(at144 >= 59 && at144 <= 61);
    // A 5-second stall drops time instead of running 300 catch-up ticks in one frame.
    uint64_t t2 = h.engine->sim_clock().tick;
    h.backend.advance_clock(5'000'000'000ull);
    h.engine->frame();
    CHECK(h.engine->sim_clock().tick - t2 == uint64_t(h.engine->config().max_ticks_per_frame));
}

#include "../engine/net/rollback.h"

TEST(net_rollback_corrects_a_late_input) {
    // Timeline A: the player jumps at tick J. Timeline B: B first simulates with the jump
    // missing (a late network packet), then learns the truth and rolls back.
    auto make_jump = [](uint64_t tick, const InputFrame& base) {
        InputFrame f = base;
        f.tick = tick;
        f.pressed = {key::Space};   // a tap: pressed and released within the tick,
        f.released = {key::Space};  // so the key is not left held on either timeline
        return f;
    };
    Harness a, b;
    a.tap(key::Enter); b.tap(key::Enter);
    a.frames(60); b.frames(60);
    CHECK(a.engine->state_hash() == b.engine->state_hash());
    const uint64_t J = a.engine->sim_clock().tick + 25;

    RollbackBuffer ra, rb(10, 600);
    for (int i = 0; i < 80; i++) {
        uint64_t t = a.engine->sim_clock().tick;
        if (t == J) { a.engine->replay_tick(make_jump(t, a.engine->input().frame())); continue; }
        ra.advance(*a.engine);
    }
    for (int i = 0; i < 80; i++) rb.advance(*b.engine);   // no jump at all
    CHECK(a.engine->state_hash() != b.engine->state_hash());

    std::string err;
    InputFrame late;
    for (const InputFrame& f : rb.log()) if (f.tick == J) late = f;
    CHECK(rb.correct(*b.engine, J, make_jump(J, late), err));
    if (!err.empty()) std::printf("    rollback error: %s\n", err.c_str());
    CHECK(rb.resimulated_ticks() > 50);
    CHECK(a.engine->sim_clock().tick == b.engine->sim_clock().tick);
    CHECK(a.engine->state_hash() == b.engine->state_hash());  // corrected timeline == true timeline
}
