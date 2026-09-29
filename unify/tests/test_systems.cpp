#include <filesystem>
#include "test_main.h"
#include "../engine/input/input.h"
#include "../engine/audio/audio.h"
#include "../engine/render/render.h"
#include "../engine/animation/animation.h"
#include "../engine/memory/memory.h"
#include "../engine/core/core.h"
#include "../engine/serialization/stream.h"
#include <cstdio>
#include <cmath>

using namespace unify;

// ------------------------------------------------------------------------------ input

static InputEvent ev(uint64_t t, InputEventType type, uint16_t code) { InputEvent e; e.timestamp_ns = t; e.type = type; e.code = code; return e; }

TEST(input_press_between_ticks_is_not_lost) {
    InputSystem in;
    in.bind("jump", key::Space);
    SimClock clock;
    // Tap lasts 3 ms, entirely between two 16.7 ms ticks.
    in.push(ev(20'000'000, InputEventType::ButtonDown, key::Space));
    in.push(ev(23'000'000, InputEventType::ButtonUp, key::Space));
    in.consume_tick(0, clock.tick_end_ns(0));   // tick 0 ends at 16.7 ms: nothing yet
    CHECK(!in.is_pressed("jump"));
    in.consume_tick(1, clock.tick_end_ns(1));   // tick 1 covers the tap
    CHECK(in.is_pressed("jump"));
    CHECK(in.is_released("jump"));
    CHECK(!in.is_held("jump") || in.is_pressed("jump"));
    in.consume_tick(2, clock.tick_end_ns(2));
    CHECK(!in.is_pressed("jump"));
}

TEST(input_future_events_stay_queued) {
    InputSystem in;
    in.bind("left", key::Left);
    in.push(ev(100'000'000, InputEventType::ButtonDown, key::Left));
    in.consume_tick(0, 16'000'000);
    CHECK(!in.is_held("left"));
    CHECK(in.queued() == 1);
    in.consume_tick(6, 116'000'000);
    CHECK(in.is_held("left") && in.is_pressed("left"));
    in.consume_tick(7, 133'000'000);
    CHECK(in.is_held("left") && !in.is_pressed("left"));
}

TEST(input_axis_repeat_pointer_and_buffered_press) {
    InputSystem in;
    in.bind("jump", key::Space);
    in.bind_axis("move", key::Left, key::Right, key::PadAxisLeftX);
    in.push(ev(1, InputEventType::ButtonDown, key::Right));
    InputEvent pad; pad.timestamp_ns = 2; pad.type = InputEventType::Axis; pad.code = key::PadAxisLeftX; pad.value = -0.5f; pad.device = InputDevice::Gamepad;
    in.push(pad);
    InputEvent mv; mv.timestamp_ns = 3; mv.type = InputEventType::PointerMove; mv.value = 120; mv.value2 = 45; mv.device = InputDevice::Mouse;
    in.push(mv);
    in.push(ev(4, InputEventType::ButtonRepeat, key::Right));
    in.push(ev(5, InputEventType::ButtonDown, key::Space));
    in.consume_tick(10, 16'000'000);
    CHECK_NEAR(in.axis("move"), 0.5f, 1e-6);
    CHECK(in.pointer() == Vec2(120, 45));
    CHECK(in.frame().repeated.size() == 1);
    for (uint64_t t = 11; t < 14; t++) in.consume_tick(t, 16'000'000 * (t - 9));
    CHECK(in.buffered("jump", 4));       // pressed 3 ticks ago: still buffered
    in.consume_buffered("jump");
    CHECK(!in.buffered("jump", 4));
}

TEST(input_frames_serialize_for_replay) {
    InputSystem in;
    in.set_recording(true);
    in.push(ev(1, InputEventType::ButtonDown, key::A));
    in.consume_tick(0, 16'000'000);
    in.consume_tick(1, 33'000'000);
    BinaryWriter w;
    in.recorded()[0].serialize(w);
    BinaryReader r(w.data());
    CHECK(InputFrame::deserialize(r) == in.recorded()[0]);
}

// ------------------------------------------------------------------------------ audio

static std::string tmp_wav(const char* name, int frames, bool loop) {
    std::vector<int16_t> pcm(size_t(frames) * 2);
    for (int i = 0; i < frames; i++) pcm[size_t(i) * 2] = pcm[size_t(i) * 2 + 1] = int16_t(10000 * std::sin(i * 0.05));
    std::string path = std::string("/tmp/unify_test_") + name + ".wav";
    write_wav(path, pcm, 2, 44100, loop ? frames / 4 : -1, loop ? frames : -1);
    return path;
}

TEST(audio_wav_round_trip_with_loop_points) {
    std::string p = tmp_wav("loop", 4000, true);
    AudioClip c;
    CHECK(c.load_wav(p));
    CHECK(c.frames() == 4000);
    CHECK(c.loop_start == 1000 && c.loop_end == 4000);
}

TEST(audio_mixer_voices_buses_pan_and_fade) {
    auto clip = std::make_shared<AudioClip>();
    clip->name = "tone";
    clip->samples.assign(44100 * 2, 0.5f);
    Mixer mx(44100, 4);
    std::vector<float> out(512 * 2);
    PlayParams p; p.pan = -1;  // hard left
    AudioHandle h = mx.play(clip, p);
    mx.render(out.data(), 512);
    CHECK(out[0] > 0.6f && std::fabs(out[1]) < 1e-5f);  // constant-power left, silent right
    mx.set_bus_volume(Bus::Sfx, 0);
    mx.render(out.data(), 512);
    CHECK(out[0] == 0);
    mx.set_bus_volume(Bus::Sfx, 1);
    mx.fade(h, 0, 0.01f);  // ~441 frames
    mx.render(out.data(), 512);
    CHECK(out[1022] == 0);
    CHECK(mx.stop(h));
    CHECK(!mx.stop(h));  // stale handle
    // Voice limit: the 5th concurrent sound is dropped, not crashing.
    for (int i = 0; i < 4; i++) CHECK(mx.play(clip));
    CHECK(!mx.play(clip));
}

TEST(audio_pitch_and_pause) {
    auto clip = std::make_shared<AudioClip>();
    clip->name = "ramp";
    for (int i = 0; i < 1000; i++) { clip->samples.push_back(i / 1000.0f); clip->samples.push_back(i / 1000.0f); }
    Mixer mx(44100, 4);
    PlayParams p; p.pitch = 2;
    AudioHandle h = mx.play(clip, p);
    std::vector<float> out(200 * 2);
    mx.render(out.data(), 100);
    CHECK_NEAR(out[2 * 50] / 1.41421356f * 1.41421356f, 0.100f * std::cos(3.14159265f / 4) * 1.41421356f, 1e-3);  // 2x speed: frame 50 plays sample 100
    mx.pause(h);
    mx.render(out.data(), 100);
    CHECK(out[0] == 0);
    mx.resume(h);
    CHECK(mx.playing(h));
    mx.render(out.data(), 200);
    mx.render(out.data(), 200);   // source position reaches exactly the end (frame 1000)
    CHECK(mx.playing(h));
    mx.render(out.data(), 1);     // first frame past the end frees the voice
    CHECK(!mx.playing(h));
}

TEST(audio_streaming_reads_incrementally_and_loops) {
    std::string p = tmp_wav("music", 44100 * 3, true);  // 3 s file, loop from 0.75 s
    auto s = std::make_shared<StreamingSource>();
    s->name = "music";
    CHECK(s->open(p));
    Mixer mx(44100, 4);
    PlayParams pp; pp.bus = Bus::Music; pp.loop = true;
    AudioHandle h = mx.stream(s, pp);
    CHECK(s->buffer.available() <= s->buffer.capacity());   // never more than 1 s buffered
    CHECK(s->decoder.position() < 44100 * 3);               // not the whole file
    std::vector<float> out(1024 * 2);
    for (int i = 0; i < 44100 * 5 / 1024; i++) { AudioStreamer::pump(mx); mx.render(out.data(), 1024); }
    CHECK(mx.playing(h));                // 5 s of playback from a 3 s file: it looped
    CHECK(s->underruns == 0);
    CHECK(s->frames_consumed > 44100 * 4);
}

TEST(audio_null_device_advances_audio_clock_on_its_own_thread) {
    Mixer mx(44100, 8);
    NullAudioDevice dev;
    dev.start(mx);
    uint64_t t0 = wall_clock_ns();
    while (mx.frames_rendered() < 4410 && wall_clock_ns() - t0 < 2'000'000'000ull) {}
    dev.stop();
    CHECK(mx.frames_rendered() >= 4410);
}

// ------------------------------------------------------------------------------ renderer

TEST(renderer_batches_by_texture_and_respects_layers) {
    SoftwareRenderDevice dev;
    Image red(4, 4, {255, 0, 0, 255}), blue(4, 4, {0, 0, 255, 255});
    uint32_t tr = dev.create_texture(red).bits, tb = dev.create_texture(blue).bits;
    Renderer2D r(dev);
    RenderTarget target(64, 64);
    Camera cam; cam.zoom = 8;
    r.begin(target, cam, {});
    for (int i = 0; i < 10; i++) r.sprite(tr, {0, 0, 4, 4}, {-2.0f + i * 0.4f, 0}, {1, 1}, 0, {}, 0, 0);
    for (int i = 0; i < 10; i++) r.sprite(tb, {0, 0, 4, 4}, {-2.0f + i * 0.4f, 2}, {1, 1}, 0, {}, 0, 0);
    r.sprite(tb, {0, 0, 4, 4}, {0, 0}, {2, 2}, 0, {}, 5, 0);  // higher layer draws on top of the red row
    r.end();
    CHECK(r.stats().commands == 21);
    // Red row, then blue row + the layer-5 blue sprite: those are adjacent in draw order with
    // identical state, so they merge without changing what is drawn on top.
    CHECK(r.stats().batches == 2);
    const uint8_t* centre = target.color.at(32, 32);
    CHECK(centre[2] == 255 && centre[0] == 0);
}

TEST(renderer_alpha_blend_scissor_and_camera) {
    SoftwareRenderDevice dev;
    Renderer2D r(dev);
    RenderTarget target(32, 32);
    Camera cam; cam.zoom = 1; cam.position = {100, 100};
    r.begin(target, cam, {}, {0, 0, 0, 255});
    r.set_scissor({0, 0, 16, 32});
    r.sprite(0, {0, 0, 1, 1}, {100, 100}, {32, 32}, 0, {255, 255, 255, 128}, 0, 0);
    r.end();
    CHECK(target.color.at(8, 16)[0] == 128);   // half-transparent white over black
    CHECK(target.color.at(24, 16)[0] == 0);    // outside the scissor
    Vec2 s = r.world_to_screen({100, 100});
    CHECK(s == Vec2(16, 16));
    CHECK(r.screen_to_world(s) == Vec2(100, 100));
}

TEST(renderer_rotation_and_atlas) {
    Image a(8, 8, {255, 0, 0, 255}), b(8, 16, {0, 255, 0, 255});
    Image packed;
    Atlas atlas;
    CHECK(pack_atlas({{"a", &a}, {"b", &b}}, 64, packed, atlas));
    CHECK(atlas.find("b") == 2);
    RectI rb = atlas.regions[size_t(atlas.find("b"))].rect;
    CHECK(packed.at(rb.x + 1, rb.y + 1)[1] == 255);
    SoftwareRenderDevice dev;
    uint32_t tex = dev.create_texture(packed).bits;
    Renderer2D r(dev);
    RenderTarget target(40, 40);
    Camera cam; cam.zoom = 1;
    r.begin(target, cam, {});
    RectI ra = atlas.regions[size_t(atlas.find("a"))].rect;
    r.sprite(tex, {float(ra.x), float(ra.y), 8, 8}, {0, 0}, {20, 20}, 0.785398f, {}, 0, 0);
    r.end();
    CHECK(target.color.at(20, 20)[0] == 255);   // centre covered
    CHECK(target.color.at(11, 11)[0] != 255);   // corner of the unrotated square is empty when rotated 45°
}

TEST(image_codecs_round_trip) {
    Image img(17, 9);
    for (int y = 0; y < 9; y++) for (int x = 0; x < 17; x++) { uint8_t* p = img.at(x, y); p[0] = uint8_t(x * 13); p[1] = uint8_t(y * 27); p[2] = uint8_t((x + y) & 1 ? 255 : 0); p[3] = uint8_t(200 + x); }
    Image back;
    CHECK(decode_qoi(encode_qoi(img), back));
    CHECK(back.pixels == img.pixels);
    const auto png_path = (std::filesystem::temp_directory_path() / "unify_test.png").string();
    CHECK(write_png(png_path, img));
    std::filesystem::remove(png_path);
}

// ------------------------------------------------------------------------------ animation

TEST(animation_modes_blend_and_sim_time) {
    AnimationSystem anim;
    AnimationClip run{"run", PlayMode::Loop, {{1, 0.1f}, {2, 0.1f}, {3, 0.1f}}, {}};
    AnimationClip bob{"bob", PlayMode::PingPong, {}, {{"y", {{0, 0}, {1, 10}}}}};
    AnimationClip jump{"jump", PlayMode::Once, {{7, 0.5f}}, {{"y", {{0, 100}, {0.5f, 100}}}}};
    anim.add_clip(run); anim.add_clip(bob); anim.add_clip(jump);
    uint32_t p = anim.create_player("run");
    for (int i = 0; i < 9; i++) anim.advance(1.0f / 60);   // 0.15 s
    CHECK(anim.sprite_region(p) == 2);
    for (int i = 0; i < 12; i++) anim.advance(1.0f / 60);  // 0.35 s -> wraps to 0.05
    CHECK(anim.sprite_region(p) == 1);
    uint32_t q = anim.create_player("bob");
    for (int i = 0; i < 90; i++) anim.advance(1.0f / 60);  // 1.5 s: ping-pong back at 0.5
    CHECK_NEAR(anim.value(q, "y", -1), 5, 1e-3);
    anim.play(q, "jump", 0.5f);
    for (int i = 0; i < 15; i++) anim.advance(1.0f / 60);  // halfway through the crossfade
    float y = anim.value(q, "y", -1);
    CHECK(y > 40 && y < 60);
    for (int i = 0; i < 60; i++) anim.advance(1.0f / 60);
    CHECK(anim.player(q)->finished);
    CHECK_NEAR(anim.value(q, "y", -1), 100, 1e-4);
}

// ------------------------------------------------------------------------------ memory

TEST(memory_allocators) {
    FrameAllocator fa(1024);
    void* a = fa.allocate(100);
    void* b = fa.allocate(100, 64);
    CHECK(a && b && (reinterpret_cast<uintptr_t>(b) % 64) == 0);
    CHECK(!fa.allocate(4096) && fa.overflows() == 1);
    fa.reset();
    CHECK(fa.used() == 0);

    PoolAllocator pool(24, 4);
    std::vector<void*> blocks;
    for (int i = 0; i < 10; i++) blocks.push_back(pool.allocate());
    CHECK(pool.in_use() == 10 && pool.capacity() == 12);
    for (void* p : blocks) pool.free(p);
    CHECK(pool.in_use() == 0);

    ObjectPool<std::string> strings;
    auto* s = strings.create("hello");
    CHECK(*s == "hello" && strings.live() == 1);
    strings.destroy(s);
    CHECK(strings.live() == 0);

    AssetAllocator assets(1000);
    uint8_t* x = assets.allocate(600, "texture");
    CHECK(x && !assets.allocate(600, "audio") && assets.failures() == 1);
    assets.free(x);
    CHECK(assets.used() == 0 && assets.live_allocations() == 0);

    BufferAllocator ba(16, 1024);
    uint32_t o1 = ba.allocate(100), o2 = ba.allocate(100), o3 = ba.allocate(100);
    CHECK(o1 == 16 && o2 > o1 && o3 > o2);
    ba.free(o2);
    CHECK(ba.allocate(64) == o2);   // first fit reuses the hole
    ba.free(o1); ba.free(o3); ba.free(o2);
    CHECK(ba.largest_free() == 1024);  // fully coalesced
}

TEST(handles_detect_stale_references) {
    HandlePool<EntityHandle> pool;
    EntityHandle a = pool.allocate();
    CHECK(pool.valid(a));
    pool.release(a);
    CHECK(!pool.valid(a));
    EntityHandle b = pool.allocate();
    CHECK(b.index() == a.index() && !pool.valid(a) && pool.valid(b));
    CHECK(!pool.valid(EntityHandle{}));
    CHECK(!pool.valid(EntityHandle::make(999, 1)));
}

TEST(rng_is_deterministic_and_serializable) {
    Rng a(42), b(42);
    for (int i = 0; i < 100; i++) CHECK(a.next_u32() == b.next_u32());
    uint32_t st[4];
    for (int i = 0; i < 4; i++) st[i] = a.state()[i];
    float x = a.next_float();
    Rng c(1);
    c.set_state(st);
    CHECK(c.next_float() == x);
    for (int i = 0; i < 1000; i++) { float f = a.next_float(); CHECK(f >= 0 && f < 1); }
}
