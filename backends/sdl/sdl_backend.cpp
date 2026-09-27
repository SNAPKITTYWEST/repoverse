#include "sdl_backend.h"
#include "core/core.h"
#include <SDL.h>

namespace unify {

// ------------------------------------------------------------------------------ audio

void SdlAudioDevice::callback(void* user, uint8_t* stream, int len) {
    auto* self = static_cast<SdlAudioDevice*>(user);
    self->mixer_->render(reinterpret_cast<float*>(stream), uint32_t(len / int(sizeof(float) * 2)));
}

bool SdlAudioDevice::start(Mixer& mixer) {
    mixer_ = &mixer;
    SDL_AudioSpec want{}, have{};
    want.freq = int(mixer.device_rate());
    want.format = AUDIO_F32SYS;
    want.channels = 2;
    want.samples = 512;
    want.callback = &SdlAudioDevice::callback;
    want.userdata = this;
    device_ = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (!device_) { UNIFY_LOG_WARN("AUDIO", "SDL audio unavailable: %s", SDL_GetError()); return false; }
    SDL_PauseAudioDevice(device_, 0);
    UNIFY_LOG_INFO("AUDIO", "SDL audio: %d Hz, %d-frame buffer", have.freq, have.samples);
    return true;
}

void SdlAudioDevice::stop() {
    if (device_) { SDL_CloseAudioDevice(device_); device_ = 0; }
}

// ------------------------------------------------------------------------------ backend

AudioDevice& SdlBackend::audio_device() { return audio_ok_ ? static_cast<AudioDevice&>(audio_) : null_audio_; }

bool SdlBackend::init(const EngineConfig& config, std::string& error) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) { error = SDL_GetError(); return false; }
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        UNIFY_LOG_WARN("AUDIO", "SDL audio subsystem failed (%s); using the null device", SDL_GetError());
        audio_ok_ = false;
    } else {
        // Probe the device now so the engine starts the right one.
        SDL_AudioSpec want{}, have{};
        want.freq = 44100; want.format = AUDIO_F32SYS; want.channels = 2; want.samples = 512;
        want.callback = [](void*, uint8_t* s, int n) { SDL_memset(s, 0, size_t(n)); };
        SDL_AudioDeviceID probe = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
        if (!probe) { audio_ok_ = false; UNIFY_LOG_WARN("AUDIO", "no audio device (%s); using the null device", SDL_GetError()); }
        else SDL_CloseAudioDevice(probe);
    }
    width_ = config.width;
    height_ = config.height;
    window_ = SDL_CreateWindow(config.title.c_str(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, width_ * 2, height_ * 2,
                               SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!window_) { error = SDL_GetError(); return false; }
    renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_PRESENTVSYNC);
    if (!renderer_) renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_SOFTWARE);
    if (!renderer_) { error = SDL_GetError(); return false; }
    SDL_RenderSetLogicalSize(renderer_, width_, height_);  // integer-scaled pixel art, letterboxed
    SDL_RenderSetIntegerScale(renderer_, SDL_TRUE);
    texture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, width_, height_);
    if (!texture_) { error = SDL_GetError(); return false; }
    for (int i = 0; i < SDL_NumJoysticks(); i++)
        if (SDL_IsGameController(i)) { pad_ = SDL_GameControllerOpen(i); break; }
    perf_freq_ = SDL_GetPerformanceFrequency();
    perf_start_ = SDL_GetPerformanceCounter();
    initialized_ = true;
    UNIFY_LOG_INFO("BOOT", "SDL %s video driver, window %dx%d", SDL_GetCurrentVideoDriver(), width_ * 2, height_ * 2);
    return true;
}

uint64_t SdlBackend::now_ns() {
    uint64_t t = SDL_GetPerformanceCounter() - perf_start_;
    return uint64_t((__uint128_t(t) * 1000000000u) / perf_freq_);
}

static uint16_t map_key(SDL_Keycode k) {
    switch (k) {
        case SDLK_SPACE: return key::Space;
        case SDLK_LEFT: return key::Left;
        case SDLK_RIGHT: return key::Right;
        case SDLK_UP: return key::Up;
        case SDLK_DOWN: return key::Down;
        case SDLK_RETURN: return key::Enter;
        case SDLK_ESCAPE: return key::Escape;
        case SDLK_F1: return key::F1;
        case SDLK_F2: return key::F2;
        case SDLK_F3: return key::F3;
        case SDLK_F5: return key::F5;
        case SDLK_F9: return key::F9;
        case SDLK_LSHIFT: return key::LShift;
        default: return (k >= 'a' && k <= 'z') ? uint16_t(k) : 0;
    }
}
static uint16_t map_pad(uint8_t b) {
    switch (b) {
        case SDL_CONTROLLER_BUTTON_A: return key::PadA;
        case SDL_CONTROLLER_BUTTON_B: return key::PadB;
        case SDL_CONTROLLER_BUTTON_X: return key::PadX;
        case SDL_CONTROLLER_BUTTON_Y: return key::PadY;
        case SDL_CONTROLLER_BUTTON_START: return key::PadStart;
        case SDL_CONTROLLER_BUTTON_BACK: return key::PadBack;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return key::PadLeft;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return key::PadRight;
        case SDL_CONTROLLER_BUTTON_DPAD_UP: return key::PadUp;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return key::PadDown;
        default: return 0;
    }
}

void SdlBackend::poll(InputSystem& input, bool& quit) {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        InputEvent e;
        e.timestamp_ns = now_ns();  // stamped on arrival: decides which simulation tick consumes it
        switch (ev.type) {
            case SDL_QUIT: quit = true; break;
            case SDL_KEYDOWN: case SDL_KEYUP:
                e.device = InputDevice::Keyboard;
                e.code = map_key(ev.key.keysym.sym);
                if (!e.code) break;
                e.type = ev.type == SDL_KEYUP ? InputEventType::ButtonUp : ev.key.repeat ? InputEventType::ButtonRepeat : InputEventType::ButtonDown;
                input.push(e);
                break;
            case SDL_MOUSEMOTION: {
                float lx, ly;
                SDL_RenderWindowToLogical(renderer_, ev.motion.x, ev.motion.y, &lx, &ly);
                e.device = InputDevice::Mouse; e.type = InputEventType::PointerMove; e.value = lx; e.value2 = ly;
                input.push(e);
                break;
            }
            case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP:
                e.device = InputDevice::Mouse;
                e.type = ev.type == SDL_MOUSEBUTTONDOWN ? InputEventType::ButtonDown : InputEventType::ButtonUp;
                e.code = ev.button.button == SDL_BUTTON_LEFT ? key::MouseLeft : ev.button.button == SDL_BUTTON_RIGHT ? key::MouseRight : key::MouseMiddle;
                input.push(e);
                break;
            case SDL_CONTROLLERBUTTONDOWN: case SDL_CONTROLLERBUTTONUP:
                e.device = InputDevice::Gamepad;
                e.type = ev.type == SDL_CONTROLLERBUTTONDOWN ? InputEventType::ButtonDown : InputEventType::ButtonUp;
                e.code = map_pad(ev.cbutton.button);
                if (e.code) input.push(e);
                break;
            case SDL_CONTROLLERAXISMOTION:
                if (ev.caxis.axis == SDL_CONTROLLER_AXIS_LEFTX || ev.caxis.axis == SDL_CONTROLLER_AXIS_LEFTY) {
                    e.device = InputDevice::Gamepad; e.type = InputEventType::Axis;
                    e.code = ev.caxis.axis == SDL_CONTROLLER_AXIS_LEFTX ? key::PadAxisLeftX : key::PadAxisLeftY;
                    e.value = ev.caxis.value / 32767.0f;
                    input.push(e);
                }
                break;
            case SDL_CONTROLLERDEVICEADDED:
                if (!pad_) pad_ = SDL_GameControllerOpen(ev.cdevice.which);
                break;
            case SDL_FINGERDOWN: case SDL_FINGERUP: case SDL_FINGERMOTION:
                e.device = InputDevice::Touch;
                e.type = InputEventType::PointerMove; e.value = ev.tfinger.x * float(width_); e.value2 = ev.tfinger.y * float(height_);
                input.push(e);
                if (ev.type != SDL_FINGERMOTION) {
                    e.type = ev.type == SDL_FINGERDOWN ? InputEventType::ButtonDown : InputEventType::ButtonUp;
                    e.code = key::Touch0;
                    input.push(e);
                }
                break;
            default: break;
        }
    }
}

void SdlBackend::present(const RenderTarget& frame) {
    SDL_UpdateTexture(texture_, nullptr, frame.color.pixels.data(), frame.width() * 4);
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
    SDL_RenderClear(renderer_);
    SDL_RenderCopy(renderer_, texture_, nullptr, nullptr);
    SDL_RenderPresent(renderer_);
}

void SdlBackend::shutdown() {
    if (!initialized_) return;
    audio_.stop();
    null_audio_.stop();
    if (pad_) SDL_GameControllerClose(pad_);
    if (texture_) SDL_DestroyTexture(texture_);
    if (renderer_) SDL_DestroyRenderer(renderer_);
    if (window_) SDL_DestroyWindow(window_);
    SDL_Quit();
    initialized_ = false;
}

}  // namespace unify
