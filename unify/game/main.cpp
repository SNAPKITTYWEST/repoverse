// UNIFY demo game launcher.
//   unify_game [--headless] [--assets DIR] [--frames N] [--screenshot out.png] [--scene NAME] [--load SLOT]
#include "runtime/engine.h"
#ifdef UNIFY_HAS_SDL
#include "sdl/sdl_backend.h"
#endif
#include <cstring>
#include <memory>

using namespace unify;

int main(int argc, char** argv) {
    EngineConfig config;
    config.title = "Repoverse - SNAPKITTY: Bifrost Runner";
    config.boot_scene = "snapkitty";
    config.asset_root = UNIFY_DEFAULT_ASSETS;
    bool headless = false;
    long frames = -1;
    std::string screenshot, load_slot;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--headless") headless = true;
        else if (a == "--assets") config.asset_root = next();
        else if (a == "--frames") frames = std::stol(next());
        else if (a == "--screenshot") screenshot = next();
        else if (a == "--scene") config.boot_scene = next();
        else if (a == "--load") load_slot = next();
        else if (a == "--seed") config.seed = std::stoull(next());
        else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
    }

    std::unique_ptr<Backend> backend;
#ifdef UNIFY_HAS_SDL
    if (!headless) backend = std::make_unique<SdlBackend>();
#endif
    auto* headless_backend = backend ? nullptr : new HeadlessBackend();
    if (!backend) backend.reset(headless_backend);

    Engine engine(*backend, config);
    std::string err;
    if (!engine.boot(err)) { std::fprintf(stderr, "boot failed: %s\n", err.c_str()); return 1; }
    if (!load_slot.empty() && !engine.load_from_file(load_slot, err)) std::fprintf(stderr, "load failed: %s\n", err.c_str());

    for (long f = 0; !engine.quit_requested() && (frames < 0 || f < frames); f++) {
        if (headless_backend) headless_backend->advance_clock(16666667);  // headless: one 60 Hz tick per frame
        engine.frame();
    }
    if (!screenshot.empty()) write_png(screenshot, engine.target().color);
    backend->shutdown();
    return 0;
}
