#pragma once
#include <cstdint>
#include <string>
#include "../render/render.h"
#include "../audio/audio.h"
#include "../input/input.h"

namespace unify {

struct EngineConfig {
    std::string title = "UNIFY";
    int width = 640, height = 360;
    uint32_t tick_rate = 60;
    uint64_t seed = 1;
    std::string asset_root = "assets";
    std::string boot_scene = "title";
    size_t lua_memory_limit = 64u << 20;
    int lua_gc_budget_kb = 32;
    int max_ticks_per_frame = 8;   // beyond this the sim drops time instead of spiralling
};

/// Platform backend. UNIFY talks to the platform only through this interface; gameplay
/// never sees it. Implementations: Headless (engine), SDL2 (backends/sdl), Unreal
/// (backends/unreal). A backend supplies a render device, an audio device, input events
/// stamped on its clock, and presentation of finished frames.
class Backend {
public:
    virtual ~Backend() = default;
    virtual const char* name() const = 0;
    virtual bool init(const EngineConfig& config, std::string& error) = 0;
    virtual RenderDevice& render_device() = 0;
    virtual AudioDevice& audio_device() = 0;
    /// Pushes pending OS events into `input`, timestamped with now_ns(). Sets quit on close.
    virtual void poll(InputSystem& input, bool& quit) = 0;
    virtual void present(const RenderTarget& frame) = 0;
    /// Monotonic platform clock (the input clock). Nondeterministic; never visible to gameplay.
    virtual uint64_t now_ns() = 0;
    virtual void shutdown() = 0;
};

/// Offscreen backend: software rendering, null audio device, programmatic input and a
/// controllable clock. Used for tests, CI, replays and servers.
class HeadlessBackend : public Backend {
public:
    const char* name() const override { return "headless"; }
    bool init(const EngineConfig&, std::string&) override { return true; }
    RenderDevice& render_device() override { return device_; }
    AudioDevice& audio_device() override { return audio_; }
    void poll(InputSystem&, bool& quit) override { quit = quit_; }
    void present(const RenderTarget& frame) override { last_frame_ = frame.color; presented_++; }
    uint64_t now_ns() override { return clock_ns_; }
    void shutdown() override { audio_.stop(); }

    void advance_clock(uint64_t ns) { clock_ns_ += ns; }
    void set_clock(uint64_t ns) { clock_ns_ = ns; }
    void request_quit() { quit_ = true; }
    const Image& last_frame() const { return last_frame_; }
    uint64_t presented() const { return presented_; }

private:
    SoftwareRenderDevice device_;
    NullAudioDevice audio_;
    uint64_t clock_ns_ = 0;
    bool quit_ = false;
    Image last_frame_;
    uint64_t presented_ = 0;
};

}  // namespace unify
