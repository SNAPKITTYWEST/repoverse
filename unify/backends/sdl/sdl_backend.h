#pragma once
#include "runtime/backend.h"

struct SDL_Window;
struct SDL_Renderer;
struct SDL_Texture;
struct _SDL_GameController;

namespace unify {

/// Hardware audio through SDL: the mixer runs inside SDL's audio callback thread.
class SdlAudioDevice : public AudioDevice {
public:
    ~SdlAudioDevice() override { stop(); }
    bool start(Mixer& mixer) override;
    void stop() override;
    const char* name() const override { return "sdl"; }
private:
    static void callback(void* user, uint8_t* stream, int len);
    uint32_t device_ = 0;
    Mixer* mixer_ = nullptr;
};

/// Native desktop backend: SDL2 window + presentation of the software-rendered frame,
/// SDL audio, and keyboard/mouse/gamepad/touch input.
class SdlBackend : public Backend {
public:
    ~SdlBackend() override { shutdown(); }
    const char* name() const override { return "sdl2"; }
    bool init(const EngineConfig& config, std::string& error) override;
    RenderDevice& render_device() override { return device_; }
    AudioDevice& audio_device() override;
    void poll(InputSystem& input, bool& quit) override;
    void present(const RenderTarget& frame) override;
    uint64_t now_ns() override;
    void shutdown() override;

private:
    SoftwareRenderDevice device_;
    SdlAudioDevice audio_;
    NullAudioDevice null_audio_;
    bool audio_ok_ = true;
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    SDL_Texture* texture_ = nullptr;
    _SDL_GameController* pad_ = nullptr;
    int width_ = 0, height_ = 0;
    uint64_t perf_freq_ = 1, perf_start_ = 0;
    bool initialized_ = false;
};

}  // namespace unify
