// Automated playthrough of the demo game on the headless backend. Walks the UNIFY
// definition of done stage by stage and verifies each one with evidence from the running
// engine (not from the script's own claims). Writes screenshots to <out-dir>.
//   unify_playtest <assets-dir> <out-dir>
#include "runtime/engine.h"
#include <lua.h>
#include <cstdio>
#include <cstring>
#include <set>

using namespace unify;

static int failures = 0;
static void check(bool ok, const char* stage, const std::string& evidence) {
    std::printf("  [%s] %-22s %s\n", ok ? "PASS" : "FAIL", stage, evidence.c_str());
    if (!ok) failures++;
}

struct Game {
    HeadlessBackend backend;
    std::unique_ptr<Engine> engine;
    std::string out;
    uint64_t frames = 0;

    void frame() {
        backend.advance_clock(16666667);  // one 60 Hz tick per frame (rounded up so frames never fall a tick short)
        engine->frame();
        frames++;
    }
    void run(int n) { for (int i = 0; i < n; i++) frame(); }
    void press(uint16_t k) { engine->inject(k, true); }
    void release(uint16_t k) { engine->inject(k, false); }
    void tap(uint16_t k) { press(k); frame(); release(k); frame(); }
    double num(const char* expr) {
        lua_State* L = engine->lua().L();
        if (!engine->lua().run(std::string("return ") + expr, "probe", 1)) return -1e9;
        double v = lua_tonumber(L, -1);
        lua_pop(L, 1);
        return v;
    }
    void shot(const char* name) { write_png(out + "/" + name, engine->target().color); }
};

static size_t distinct_colors(const Image& img) {
    std::set<uint32_t> colors;
    for (size_t i = 0; i < img.pixels.size() && colors.size() < 1000; i += 4 * 7)
        colors.insert(uint32_t(img.pixels[i]) | uint32_t(img.pixels[i + 1]) << 8 | uint32_t(img.pixels[i + 2]) << 16);
    return colors.size();
}

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: unify_playtest <assets-dir> <out-dir>\n"); return 2; }
    set_log_level(LogLevel::Warn);
    Game g;
    g.out = argv[2];
    EngineConfig cfg;
    cfg.asset_root = argv[1];
    const bool snapkitty = argc > 3 && std::string(argv[3]) == "--snapkitty";
    if (snapkitty) cfg.boot_scene = "snapkitty";
    cfg.seed = 2026;
    g.engine = std::make_unique<Engine>(g.backend, cfg);
    Engine& e = *g.engine;
    std::printf("UNIFY definition-of-done playtest (headless backend)\n");

    // BOOT
    std::string err;
    bool booted = e.boot(err);
    check(booted, "BOOT", booted ? std::string(Engine::kEngineVersion) + " on " + e.backend().name() : err);
    if (!booted) return 1;

    // LOAD ASSETS (title scene: script, vadd kernel source, streamed music header)
    g.run(30);
    check(e.assets().live() >= 3, "LOAD ASSETS", std::to_string(e.assets().live()) + " assets resident, " + std::to_string(e.assets().resident_bytes()) + " bytes");
    if (!snapkitty) {
    bool vadd_ok = false;
    for (auto& l : e.log_lines()) if (l.find("vadd kernel: OK") != std::string::npos) vadd_ok = true;
    check(snapkitty || (vadd_ok && g.num("scene.state().kernel_ok and 1 or 0") == 1), "EXECUTE WASM KERNEL",
          "title: C[i]=A[i]+B[i] verified in Lua after " + std::to_string(int(g.num("scene.state().kernel_instructions"))) + " WASM instructions");
    } else {
        check(e.assets().find("textures/snapkitty/kitten").bits != 0, "SNAPKITTY FRONTEND", "original kitten texture loaded");
    }
    g.shot("01_title.png");

    // CREATE WORLD
    g.tap(key::Enter);
    g.run(2);
    bool in_level = e.active_scene() && e.active_scene()->name == (snapkitty ? "snapkitty_level" : "level");
    check(in_level && e.world().alive() > 150 && e.physics().body_count() > 20, "CREATE WORLD",
          "scene '" + std::string(e.active_scene() ? e.active_scene()->name : "?") + "': " + std::to_string(e.world().alive()) + " entities, " +
          std::to_string(e.physics().body_count()) + " bodies");

    // RUN PHYSICS: the player drops onto the ground and comes to rest.
    g.run(60);
    double py0 = g.num("select(2, scene.state().player:position())");
    check(g.num("scene.state().grounded and 1 or 0") == 1 && e.physics().stats.touching > 0, "RUN PHYSICS",
          "player resting at y=" + std::to_string(py0) + ", " + std::to_string(e.physics().stats.touching) + " touching contacts");
    g.shot("02_level_start.png");

    // ACCEPT INPUT + RUN GAMEPLAY: hold right.
    double x0 = g.num("scene.state().player:position()");
    g.press(key::Right);
    g.run(20);
    double x1 = g.num("scene.state().player:position()");
    std::string anim_run = [&] { lua_State* L = e.lua().L(); e.lua().run("return (scene.state().player:animation())", "probe", 1); std::string s = lua_tostring(L, -1) ? lua_tostring(L, -1) : ""; lua_pop(L, 1); return s; }();
    check(x1 > x0 + 1.0, "ACCEPT INPUT", "held RIGHT for 20 ticks: x " + std::to_string(x0) + " -> " + std::to_string(x1));

    // RUN LUA: gameplay counters live in Lua state and are driven by the controller script.
    check(g.num("scene.state().time") > 1.0 && e.lua().bytes() > 0, "RUN LUA",
          "scene time " + std::to_string(g.num("scene.state().time")) + "s, Lua heap " + std::to_string(e.lua().bytes() / 1024) + " KB (separate budget)");

    // ANIMATE: running clip selected from simulation state.
    check(anim_run == "player_run", "ANIMATE", "player clip while moving: " + anim_run);

    // Tap JUMP between two ticks: the press and release both land inside one tick's window
    // and must still produce a jump (input buffering).
    uint64_t jump_ns = e.next_tick_input_ns() + 2'000'000;
    e.input().push({jump_ns, InputDevice::Keyboard, InputEventType::ButtonDown, key::Space, 0, 0});
    e.input().push({jump_ns + 3'000'000, InputDevice::Keyboard, InputEventType::ButtonUp, key::Space, 0, 0});
    uint32_t voices_before = e.mixer().active_voices();
    g.run(2);
    double vy = g.num("select(2, scene.state().player:velocity())");
    check(g.num("scene.state().jumps") == 1 && vy > 0, "INPUT BUFFER", "3 ms tap between ticks -> jump, vy=" + std::to_string(vy));
    std::string anim_jump = [&] { lua_State* L = e.lua().L(); e.lua().run("return (scene.state().player:animation())", "probe", 1); std::string s = lua_tostring(L, -1) ? lua_tostring(L, -1) : ""; lua_pop(L, 1); return s; }();
    check(anim_jump == "player_jump", "ANIMATE", "jump clip with squash/stretch track: " + anim_jump);

    // PLAY AUDIO: jump SFX voice + streamed music on the audio device thread.
    uint64_t audio_frames = e.mixer().frames_rendered();
    check(e.mixer().active_voices() >= voices_before && e.mixer().active_voices() >= 2 && audio_frames > 0, "PLAY AUDIO",
          std::to_string(e.mixer().active_voices()) + " voices (music stream + SFX), audio clock " + std::to_string(e.mixer().audio_time()) + "s");
    g.shot("03_jump.png");

    // COLLIDE: keep running right, hopping every 40 ticks, until a coin is collected.
    int ticks = 0;
    while (g.num("scene.state().coins") < 1 && ticks < 900) {
        if (ticks % 40 == 0) g.press(key::Space);
        if (ticks % 40 == 12) g.release(key::Space);
        g.frame();
        ticks++;
    }
    check(g.num("scene.state().coins") >= 1 && g.num("scene.state().collisions") > 0, "COLLIDE",
          "coin trigger after " + std::to_string(ticks) + " ticks; " + std::to_string(int(g.num("scene.state().collisions"))) + " contact-begin events");
    g.run(3);
    check(g.num("scene.state().kernel_runs") > 0 && g.num("scene.state().particles_alive") > 0, "EXECUTE WASM KERNEL",
          "particles kernel dispatched " + std::to_string(int(g.num("scene.state().kernel_runs"))) + "x, " +
          std::to_string(int(g.num("scene.state().particles_alive"))) + " live particles");
    g.shot("04_coin_particles.png");

    // RENDER
    const Image& frame = g.backend.last_frame();
    check(g.backend.presented() == g.frames && distinct_colors(frame) > 20 && e.renderer().stats().batches > 0, "RENDER",
          std::to_string(e.renderer().stats().quads) + " sprites in " + std::to_string(e.renderer().stats().batches) + " batches, " +
          std::to_string(distinct_colors(frame)) + "+ colours, " + std::to_string(g.backend.presented()) + " frames presented");

    // SAVE (through the gameplay path: F5 in Lua -> save_state() at tick end)
    g.tap(key::F5);
    std::vector<uint8_t> quick = e.quick_slot();
    std::string saved_msg = [&] { lua_State* L = e.lua().L(); e.lua().run("return scene.state().msg", "probe", 1); std::string s = lua_tostring(L, -1) ? lua_tostring(L, -1) : ""; lua_pop(L, 1); return s; }();
    // Reference hash: a snapshot taken at the same tick as the quick save.
    std::vector<uint8_t> snap = e.snapshot();
    uint64_t h_save = e.state_hash();
    double score_saved = g.num("scene.state().score"), x_saved = g.num("scene.state().player:position()");
    check(!quick.empty() && saved_msg == "STATE SAVED", "SAVE", std::to_string(snap.size()) + "-byte save state at tick " + std::to_string(e.sim_clock().tick) +
          ", hash " + std::to_string(h_save));

    // CONTINUE: play on, recording the exact per-tick input the simulation consumed.
    e.input().set_recording(true);
    g.press(key::Left);
    for (int i = 0; i < 90; i++) {
        if (i == 20) g.press(key::Space);
        if (i == 30) g.release(key::Space);
        g.frame();
    }
    g.release(key::Left);
    g.frame();
    std::vector<InputFrame> recorded = e.input().recorded();
    e.input().set_recording(false);
    uint64_t h_continue = e.state_hash();
    double x_continue = g.num("scene.state().player:position()");
    check(h_continue != h_save && x_continue != x_saved, "CONTINUE", std::to_string(recorded.size()) + " more ticks; player moved to x=" + std::to_string(x_continue));

    // RESTORE
    bool restored = e.restore(snap, err);
    uint64_t h_restored = e.state_hash();
    check(restored && h_restored == h_save && g.num("scene.state().score") == score_saved && g.num("scene.state().player:position()") == x_saved,
          "RESTORE", restored ? "hash after restore " + std::to_string(h_restored) + (h_restored == h_save ? " == saved" : " != saved") : err);

    // VERIFY STATE: replaying the recorded input from the restored state reproduces the
    // continued run bit-for-bit (deterministic simulation; the rollback/lockstep property).
    for (const InputFrame& f : recorded) e.replay_tick(f);
    uint64_t h_replayed = e.state_hash();
    check(h_replayed == h_continue, "VERIFY STATE", "replayed " + std::to_string(recorded.size()) + " recorded input frames: hash " + std::to_string(h_replayed) +
          (h_replayed == h_continue ? " == continued run" : " != continued run " + std::to_string(h_continue)));

    // Save file round trip through disk (F5 also wrote slot1).
    std::vector<uint8_t> file;
    bool have_file = read_file(e.assets().path_for(AssetType::SaveData, "saves/slot1"), file);
    check(have_file && file.size() > 1000, "SAVE (file)", "saves/slot1.usav " + std::to_string(file.size()) + " bytes, written atomically");

    // CONTINUE PLAYING after the restore, through the normal real-time path.
    g.press(key::Right);
    g.run(120);
    g.release(key::Right);
    g.run(30);
    int errors = 0;
    for (auto& l : e.log_lines()) if (l.rfind("ERROR", 0) == 0) errors++;
    check(errors == 0 && e.physics().stats.failures == 0, "CONTINUE PLAYING",
          "tick " + std::to_string(e.sim_clock().tick) + ", " + std::to_string(errors) + " script errors, " + std::to_string(e.physics().stats.failures) + " physics failures");
    g.shot("05_after_restore.png");

    // Scheduler order for one real-time frame.
    std::string order;
    for (Stage s : e.trace()) { if (!order.empty()) order += " > "; order += stage_name(s); }
    std::printf("\n  scheduler: %s\n", order.c_str());
    std::printf("  heap allocations in last tick: %llu, Lua heap %zu KB (peak %zu KB)\n",
                static_cast<unsigned long long>(e.stats().heap_allocs_last_tick), e.lua().bytes() / 1024, e.lua().peak_bytes() / 1024);

    std::printf("\n%s: %d failure(s)\n", failures ? "FAILED" : "ALL STAGES PASSED", failures);
    g.backend.shutdown();
    return failures ? 1 : 0;
}
