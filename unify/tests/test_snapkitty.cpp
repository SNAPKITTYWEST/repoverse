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

TEST(snapkitty_disk_save_survives_engine_restart) {
    HeadlessBackend first;
    EngineConfig cfg; cfg.asset_root=UNIFY_GAME_ASSETS; cfg.boot_scene="snapkitty";
    std::string error;
    {
        Engine engine(first,cfg);
        CHECK(engine.boot(error));
        auto frame=[&]() {first.advance_clock(16666667);engine.frame();};
        auto tap=[&](uint16_t code) {engine.inject(code,true);frame();engine.inject(code,false);frame();};
        frame(); tap(key::Enter);
        CHECK(engine.lua().run("scene.state().score = 7300", "test"));
        tap(key::F5);
    }
    HeadlessBackend second;
    Engine engine(second,cfg);
    CHECK(engine.boot(error));
    auto frame=[&]() {second.advance_clock(16666667);engine.frame();};
    frame(); engine.inject(key::F9,true); frame();engine.inject(key::F9,false);frame();
    CHECK(engine.active_scene()->name == "snapkitty_level");
    CHECK(engine.lua().run("assert(scene.state().score == 7300)", "test"));
    CHECK(engine.lua().run("scene.state().score = 0", "test"));
    engine.inject(key::F9,true);frame();engine.inject(key::F9,false);frame();
    CHECK(engine.lua().run("assert(scene.state().score == 7300)", "test"));
}

TEST(snapkitty_finish_returns_to_menu) {
    HeadlessBackend backend;
    EngineConfig cfg; cfg.asset_root=UNIFY_GAME_ASSETS; cfg.boot_scene="snapkitty_level";
    Engine engine(backend,cfg);std::string error;CHECK(engine.boot(error));
    CHECK(engine.lua().run(R"lua(
        local m=map.load("maps/level1")
        for r=1,m.height do
            local x=m.rows[r]:find("F",1,true)
            if x then scene.state().player:set_position(x-.5,m.height-r+1,true); break end
        end
    )lua", "test"));
    for(int i=0;i<5;i++){backend.advance_clock(16666667);engine.frame();}
    CHECK(engine.lua().run("assert(scene.state().won and scene.state().score >= 1000)", "test"));
    for(int i=0;i<250;i++){backend.advance_clock(16666667);engine.frame();}
    CHECK(engine.active_scene()->name == "snapkitty");
}
