#include "test_main.h"
#include "runtime/engine.h"
#include <lua.h>
using namespace unify;
TEST(snapkitty_menu_repository_and_architect) {
    HeadlessBackend backend;
    EngineConfig cfg; cfg.asset_root=UNIFY_GAME_ASSETS; cfg.boot_scene="snapkitty";
    Engine engine(backend,cfg);
    std::string error;
    CHECK(engine.boot(error));
    auto frame=[&]() {backend.advance_clock(16666667);engine.frame();};
    auto tap=[&](uint16_t code) {engine.inject(code,true);frame();engine.inject(code,false);frame();};
    frame();
    CHECK(engine.assets().find("textures/snapkitty/kitten").bits != 0);
    tap(key::I);
    CHECK(engine.lua().run("assert(scene.state().info == true)", "test"));
    tap(key::R);
    CHECK(engine.active_scene()->name == "repositories");
    frame();
    CHECK(engine.assets().find("textures/snapkitty/snap-os").bits != 0);
    tap(key::Escape);
    CHECK(engine.active_scene()->name == "snapkitty");
    tap(key::Enter);
    CHECK(engine.active_scene()->name == "snapkitty_level");
    CHECK(engine.lua().run("local s=scene.state(); local x,y=s.architect:position(); s.player:set_position(x,y,true)", "test"));
    tap(key::E);
    CHECK(engine.lua().run("assert(scene.state().msg == 'SEAL THE EVENTS. REACH THE TERMINAL.')", "test"));
    for (const auto& line:engine.log_lines()) CHECK(line.rfind("ERROR",0) != 0);
}
