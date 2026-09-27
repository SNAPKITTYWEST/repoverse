// The UNIFY Lua API. Gameplay scripts reach the engine only through these functions.
// Objects cross the boundary as handles (entity, buffer, kernel, voice ids) and every
// call validates its handle; Lua never receives an engine pointer.
#include "engine.h"
#include <lua.h>
#include <lauxlib.h>
#include <algorithm>
#include <cstring>

namespace unify {

namespace {

Engine& E(lua_State* L) { return *static_cast<Engine*>(lua_touserdata(L, lua_upvalueindex(1))); }

EntityHandle check_entity(lua_State* L, int idx) {
    auto* p = static_cast<uint32_t*>(luaL_checkudata(L, idx, kEntityMeta));
    EntityHandle e{*p};
    if (!E(L).world().valid(e)) luaL_error(L, "stale entity handle (entity was destroyed)");
    return e;
}
void push_entity(lua_State* L, EntityHandle e) {
    if (!e) { lua_pushnil(L); return; }
    *static_cast<uint32_t*>(lua_newuserdatauv(L, sizeof(uint32_t), 0)) = e.bits;
    luaL_setmetatable(L, kEntityMeta);
}
PhysicsBodyHandle body_of(lua_State* L, EntityHandle e) {
    BodyC* b = E(L).world().bodies.get(e);
    return b ? PhysicsBodyHandle{b->body} : PhysicsBodyHandle{};
}
float num(lua_State* L, int i) { return float(luaL_checknumber(L, i)); }
float opt(lua_State* L, int i, float d) { return float(luaL_optnumber(L, i, d)); }

// Ensures the active scene holds a reference to an asset (so unloading the scene frees it).
AssetHandle scene_asset(lua_State* L, AssetType type, const std::string& name) {
    Engine& e = E(L);
    AssetHandle h = e.assets().find(name);
    Scene* s = e.current_scene();
    if (!h) {
        h = e.assets().load(type, name);
        if (!h) luaL_error(L, "%s asset '%s' could not be loaded", asset_type_name(type), name.c_str());
        if (s) s->assets.push_back(h); else e.assets().acquire(h);
    } else if (s && std::find(s->assets.begin(), s->assets.end(), h) == s->assets.end()) {
        e.assets().acquire(h);
        s->assets.push_back(h);
    }
    return h;
}

// ------------------------------------------------------------------------------ entity
int ent_set_position(lua_State* L) {
    EntityHandle e = check_entity(L, 1);
    Vec2 p{num(L, 2), num(L, 3)};
    TransformC* t = E(L).world().transforms.get(e);
    t->current.position = p;
    if (lua_toboolean(L, 4)) t->previous.position = p;  // teleport: no interpolation smear
    if (PhysicsBodyHandle b = body_of(L, e)) E(L).physics().set_transform(b, p, t->current.rotation);
    return 0;
}
int ent_position(lua_State* L) {
    EntityHandle e = check_entity(L, 1);
    Vec2 p = E(L).world().transforms.get(e)->current.position;
    lua_pushnumber(L, p.x); lua_pushnumber(L, p.y);
    return 2;
}
int ent_set_velocity(lua_State* L) {
    EntityHandle e = check_entity(L, 1);
    PhysicsBodyHandle b = body_of(L, e);
    if (!b) return luaL_error(L, "entity has no physics body");
    E(L).physics().set_velocity(b, {num(L, 2), num(L, 3)});
    return 0;
}
int ent_velocity(lua_State* L) {
    EntityHandle e = check_entity(L, 1);
    const Body* b = E(L).physics().get(body_of(L, e));
    lua_pushnumber(L, b ? b->velocity.x : 0);
    lua_pushnumber(L, b ? b->velocity.y : 0);
    return 2;
}
int ent_apply_impulse(lua_State* L) {
    EntityHandle e = check_entity(L, 1);
    E(L).physics().apply_impulse(body_of(L, e), {num(L, 2), num(L, 3)});
    return 0;
}
int ent_set_rotation(lua_State* L) {
    EntityHandle e = check_entity(L, 1);
    TransformC* t = E(L).world().transforms.get(e);
    t->current.rotation = num(L, 2);
    if (PhysicsBodyHandle b = body_of(L, e)) E(L).physics().set_transform(b, t->current.position, t->current.rotation);
    return 0;
}
int ent_rotation(lua_State* L) { lua_pushnumber(L, E(L).world().transforms.get(check_entity(L, 1))->current.rotation); return 1; }
int ent_set_animation(lua_State* L) {
    EntityHandle e = check_entity(L, 1);
    AnimatorC* a = E(L).world().animators.get(e);
    const char* clip = luaL_checkstring(L, 2);
    if (!E(L).animation().clip(clip)) return luaL_error(L, "unknown animation '%s'", clip);
    if (!a) a = &E(L).world().animators.add(e, AnimatorC{E(L).animation().create_player(clip)});
    E(L).animation().play(a->player, clip, opt(L, 3, 0), lua_toboolean(L, 4));
    return 0;
}
int ent_animation(lua_State* L) {
    AnimatorC* a = E(L).world().animators.get(check_entity(L, 1));
    AnimationPlayer* p = a ? E(L).animation().player(a->player) : nullptr;
    if (!p) { lua_pushnil(L); return 1; }
    lua_pushstring(L, p->clip.c_str());
    lua_pushboolean(L, p->finished);
    return 2;
}
SpriteC& sprite_of(lua_State* L, EntityHandle e) {
    SpriteC* s = E(L).world().sprites.get(e);
    if (!s) luaL_error(L, "entity has no sprite");
    return *s;
}
int ent_set_flip(lua_State* L) { sprite_of(L, check_entity(L, 1)).flip_x = lua_toboolean(L, 2); return 0; }
int ent_set_visible(lua_State* L) { sprite_of(L, check_entity(L, 1)).visible = lua_toboolean(L, 2); return 0; }
int ent_set_tint(lua_State* L) {
    SpriteC& s = sprite_of(L, check_entity(L, 1));
    s.tint = {uint8_t(luaL_checkinteger(L, 2)), uint8_t(luaL_checkinteger(L, 3)), uint8_t(luaL_checkinteger(L, 4)), uint8_t(luaL_optinteger(L, 5, 255))};
    return 0;
}
int ent_name(lua_State* L) { lua_pushstring(L, E(L).world().info(check_entity(L, 1))->name.c_str()); return 1; }
int ent_valid(lua_State* L) {
    auto* p = static_cast<uint32_t*>(luaL_checkudata(L, 1, kEntityMeta));
    lua_pushboolean(L, E(L).world().valid(EntityHandle{*p}));
    return 1;
}
int ent_id(lua_State* L) { lua_pushinteger(L, check_entity(L, 1).bits); return 1; }
int ent_destroy(lua_State* L) { E(L).destroy_entity(check_entity(L, 1)); return 0; }
int ent_eq(lua_State* L) {
    auto* a = static_cast<uint32_t*>(luaL_checkudata(L, 1, kEntityMeta));
    auto* b = static_cast<uint32_t*>(luaL_checkudata(L, 2, kEntityMeta));
    lua_pushboolean(L, *a == *b);
    return 1;
}
int ent_tostring(lua_State* L) {
    auto* p = static_cast<uint32_t*>(luaL_checkudata(L, 1, kEntityMeta));
    EntityHandle e{*p};
    const EntityInfo* in = E(L).world().info(e);
    lua_pushfstring(L, "Entity(%d:%d %s)", int(e.index()), int(e.generation()), in ? in->name.c_str() : "<destroyed>");
    return 1;
}

// ------------------------------------------------------------------------------ world
int world_spawn(lua_State* L) {
    std::string err;
    EntityHandle e = E(L).instantiate(luaL_checkstring(L, 2), {num(L, 3), num(L, 4)}, lua_toboolean(L, 5), err);
    if (!e) return luaL_error(L, "spawn failed: %s", err.c_str());
    push_entity(L, e);
    return 1;
}
int world_destroy(lua_State* L) { lua_pushboolean(L, E(L).destroy_entity(check_entity(L, 2))); return 1; }
int world_find(lua_State* L) { push_entity(L, E(L).world().find(luaL_checkstring(L, 2))); return 1; }
int world_count(lua_State* L) { lua_pushinteger(L, E(L).world().alive()); return 1; }

// ------------------------------------------------------------------------------ physics
int phys_apply_force(lua_State* L) {
    PhysicsBodyHandle b = body_of(L, check_entity(L, 2));
    if (!b) return luaL_error(L, "entity has no physics body");
    E(L).physics().apply_force(b, {num(L, 3), num(L, 4)});
    return 0;
}
int phys_apply_impulse(lua_State* L) { E(L).physics().apply_impulse(body_of(L, check_entity(L, 2)), {num(L, 3), num(L, 4)}); return 0; }
int phys_raycast(lua_State* L) {
    uint32_t mask = uint32_t(luaL_optinteger(L, 6, 0xFFFF));
    PhysicsBodyHandle ignore;
    if (lua_isuserdata(L, 7)) ignore = body_of(L, check_entity(L, 7));
    RayHit hit;
    if (!E(L).physics().raycast({num(L, 2), num(L, 3)}, {num(L, 4), num(L, 5)}, mask, hit, ignore)) { lua_pushboolean(L, 0); return 1; }
    lua_pushboolean(L, 1);
    lua_pushnumber(L, hit.point.x); lua_pushnumber(L, hit.point.y);
    lua_pushnumber(L, hit.normal.x); lua_pushnumber(L, hit.normal.y);
    const Body* b = E(L).physics().get(hit.body);
    push_entity(L, b && E(L).world().valid(EntityHandle{b->user}) ? EntityHandle{b->user} : EntityHandle{});
    return 6;
}
int phys_set_gravity(lua_State* L) { E(L).physics().gravity = {num(L, 2), num(L, 3)}; return 0; }
int phys_add_static_box(lua_State* L) {
    // Static collision without a sprite (e.g. merged tile runs). Owned by the active scene.
    Engine& e = E(L);
    Scene* s = e.current_scene();
    EntityHandle ent = e.world().create("static", s ? s->id : 0);
    Vec2 c{num(L, 2), num(L, 3)};
    e.world().transforms.get(ent)->current.position = c;
    e.world().transforms.get(ent)->previous.position = c;
    BodyDef d;
    d.type = BodyType::Static;
    d.shape = Shape::aabb(num(L, 4) / 2, num(L, 5) / 2);
    d.position = c;
    d.friction = opt(L, 7, 0.8f);
    d.category = uint32_t(luaL_optinteger(L, 6, 1));
    d.user = ent.bits;
    e.world().bodies.add(ent, BodyC{e.physics().create_body(d).bits});
    push_entity(L, ent);
    return 1;
}
int phys_is_awake(lua_State* L) {
    const Body* b = E(L).physics().get(body_of(L, check_entity(L, 2)));
    lua_pushboolean(L, b && b->awake);
    return 1;
}

// ------------------------------------------------------------------------------ audio
int audio_play(lua_State* L) {
    Engine& e = E(L);
    AssetHandle h = scene_asset(L, AssetType::Audio, luaL_checkstring(L, 2));
    PlayParams p;
    p.bus = Bus::Sfx; p.volume = opt(L, 3, 1); p.pan = opt(L, 4, 0); p.pitch = opt(L, 5, 1);
    AudioHandle v = e.mixer().play(e.assets().get(h)->clip, p);
    if (v && e.current_scene()) e.current_scene()->voices.push_back(v);
    lua_pushinteger(L, v.bits);
    return 1;
}
int audio_stream(lua_State* L) {
    Engine& e = E(L);
    AssetHandle h = scene_asset(L, AssetType::Music, luaL_checkstring(L, 2));
    auto src = e.assets().open_stream(h);
    if (!src) return luaL_error(L, "could not open stream '%s'", luaL_checkstring(L, 2));
    PlayParams p;
    p.bus = Bus::Music; p.volume = opt(L, 3, 1);
    p.loop = lua_isnoneornil(L, 4) ? true : lua_toboolean(L, 4);
    p.fade_in = opt(L, 5, 0);
    AudioHandle v = e.mixer().stream(src, p);
    if (v && e.current_scene()) e.current_scene()->voices.push_back(v);
    lua_pushinteger(L, v.bits);
    return 1;
}
AudioHandle voice(lua_State* L, int i) { return AudioHandle{uint32_t(luaL_checkinteger(L, i))}; }
int audio_stop(lua_State* L) { lua_pushboolean(L, E(L).mixer().stop(voice(L, 2), opt(L, 3, 0))); return 1; }
int audio_pause(lua_State* L) { lua_pushboolean(L, E(L).mixer().pause(voice(L, 2))); return 1; }
int audio_resume(lua_State* L) { lua_pushboolean(L, E(L).mixer().resume(voice(L, 2))); return 1; }
int audio_fade(lua_State* L) { lua_pushboolean(L, E(L).mixer().fade(voice(L, 2), num(L, 3), num(L, 4))); return 1; }
int audio_playing(lua_State* L) { lua_pushboolean(L, E(L).mixer().playing(voice(L, 2))); return 1; }
int audio_set_bus_volume(lua_State* L) {
    std::string bus = luaL_checkstring(L, 2);
    Bus b = bus == "music" ? Bus::Music : bus == "sfx" ? Bus::Sfx : Bus::Master;
    E(L).mixer().set_bus_volume(b, num(L, 3));
    return 0;
}

// ------------------------------------------------------------------------------ input
int in_pressed(lua_State* L) { lua_pushboolean(L, E(L).input().is_pressed(luaL_checkstring(L, 2))); return 1; }
int in_held(lua_State* L) { lua_pushboolean(L, E(L).input().is_held(luaL_checkstring(L, 2))); return 1; }
int in_released(lua_State* L) { lua_pushboolean(L, E(L).input().is_released(luaL_checkstring(L, 2))); return 1; }
int in_axis(lua_State* L) { lua_pushnumber(L, E(L).input().axis(luaL_checkstring(L, 2))); return 1; }
int in_buffered(lua_State* L) { lua_pushboolean(L, E(L).input().buffered(luaL_checkstring(L, 2), uint32_t(luaL_optinteger(L, 3, 6)))); return 1; }
int in_consume(lua_State* L) { E(L).input().consume_buffered(luaL_checkstring(L, 2)); return 0; }
int in_pointer(lua_State* L) { Vec2 p = E(L).input().pointer(); lua_pushnumber(L, p.x); lua_pushnumber(L, p.y); return 2; }
uint16_t key_code(const std::string& n) {
    static const std::map<std::string, uint16_t> keys = {
        {"space", key::Space}, {"left", key::Left}, {"right", key::Right}, {"up", key::Up}, {"down", key::Down},
        {"e", key::E}, {"i", key::I}, {"a", key::A}, {"d", key::D}, {"w", key::W}, {"s", key::S}, {"r", key::R}, {"p", key::P},
        {"enter", key::Enter}, {"escape", key::Escape}, {"f1", key::F1}, {"f2", key::F2}, {"f3", key::F3},
        {"f5", key::F5}, {"f9", key::F9}, {"shift", key::LShift}, {"mouse_left", key::MouseLeft},
        {"pad_a", key::PadA}, {"pad_b", key::PadB}, {"pad_x", key::PadX}, {"pad_y", key::PadY}, {"pad_start", key::PadStart},
        {"pad_left", key::PadLeft}, {"pad_right", key::PadRight}, {"pad_up", key::PadUp}, {"pad_down", key::PadDown},
        {"pad_left_x", key::PadAxisLeftX}, {"pad_left_y", key::PadAxisLeftY}, {"touch", key::Touch0}};
    auto it = keys.find(n);
    return it == keys.end() ? 0 : it->second;
}
int in_bind(lua_State* L) {
    const char* action = luaL_checkstring(L, 2);
    for (int i = 3; i <= lua_gettop(L); i++) {
        uint16_t c = key_code(luaL_checkstring(L, i));
        if (!c) return luaL_error(L, "unknown key '%s'", lua_tostring(L, i));
        E(L).input().bind(action, c);
    }
    return 0;
}
int in_bind_axis(lua_State* L) {
    uint16_t neg = key_code(luaL_checkstring(L, 3)), pos = key_code(luaL_checkstring(L, 4));
    uint16_t analog = lua_isstring(L, 5) ? key_code(lua_tostring(L, 5)) : 0;
    if (!neg || !pos) return luaL_error(L, "unknown key in axis binding");
    E(L).input().bind_axis(luaL_checkstring(L, 2), neg, pos, analog);
    return 0;
}

// ------------------------------------------------------------------------------ camera / ui
int cam_set(lua_State* L) {
    Camera& c = E(L).camera();
    c.position = {num(L, 2), num(L, 3)};
    if (!lua_isnoneornil(L, 4)) c.zoom = num(L, 4);
    return 0;
}
int cam_get(lua_State* L) {
    const Camera& c = E(L).camera();
    lua_pushnumber(L, c.position.x); lua_pushnumber(L, c.position.y); lua_pushnumber(L, c.zoom);
    return 3;
}
void require_render(lua_State* L) {
    if (!E(L).in_render_phase()) luaL_error(L, "ui calls are only valid inside render()");
}
Color color_args(lua_State* L, int i) {
    return {uint8_t(luaL_optinteger(L, i, 255)), uint8_t(luaL_optinteger(L, i + 1, 255)), uint8_t(luaL_optinteger(L, i + 2, 255)), uint8_t(luaL_optinteger(L, i + 3, 255))};
}
int ui_text(lua_State* L) {
    require_render(L);
    E(L).font().draw(E(L).renderer(), luaL_checkstring(L, 2), {num(L, 3), num(L, 4)}, opt(L, 5, 2), color_args(L, 6), 1000);
    return 0;
}
int ui_rect(lua_State* L) {
    require_render(L);
    E(L).renderer().fill_screen_rect({num(L, 2), num(L, 3), num(L, 4), num(L, 5)}, color_args(L, 6), 999);
    return 0;
}
int ui_image(lua_State* L) {
    require_render(L);
    const char* name = luaL_checkstring(L, 2);
    const TextureAsset* tex = E(L).texture(name);
    if (!tex) return luaL_error(L, "image '%s' could not be loaded", name);
    E(L).renderer().screen_rect(tex->texture.bits, {0, 0, float(tex->width), float(tex->height)},
        {num(L, 3), num(L, 4), num(L, 5), num(L, 6)}, color_args(L, 7), int16_t(luaL_optinteger(L, 11, 998)));
    return 0;
}
int ui_width(lua_State* L) { lua_pushnumber(L, E(L).font().width(luaL_checkstring(L, 2), opt(L, 3, 2))); return 1; }
int ui_screen(lua_State* L) { lua_pushinteger(L, E(L).config().width); lua_pushinteger(L, E(L).config().height); return 2; }

// ------------------------------------------------------------------------------ timer / scene / engine
int timer_after(lua_State* L) { E(L).add_timer(luaL_checknumber(L, 1), false, luaL_checkstring(L, 2)); return 0; }
int timer_every(lua_State* L) { E(L).add_timer(luaL_checknumber(L, 1), true, luaL_checkstring(L, 2)); return 0; }
int scene_transition(lua_State* L) {
    // Deferred to the end of the gameplay stage: never tear down a scene from inside its own callback.
    E(L).request_transition(luaL_checkstring(L, 1));
    return 0;
}
int scene_state(lua_State* L) {
    // The active scene's persistent state table (read-only inspection for tools and tests).
    const Scene* s = E(L).active_scene();
    if (!s || s->lua_ref < 0) { lua_pushnil(L); return 1; }
    lua_rawgeti(L, LUA_REGISTRYINDEX, s->lua_ref);
    lua_getfield(L, -1, "state");
    return 1;
}
int scene_current(lua_State* L) { const Scene* s = E(L).active_scene(); lua_pushstring(L, s ? s->name.c_str() : ""); return 1; }
int engine_tick(lua_State* L) { lua_pushinteger(L, lua_Integer(E(L).sim_clock().tick)); return 1; }
int engine_time(lua_State* L) { lua_pushnumber(L, E(L).sim_clock().seconds()); return 1; }
int engine_quit(lua_State* L) { E(L).request_quit(); return 0; }
int engine_hash(lua_State* L) {
    char buf[20];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(E(L).state_hash()));
    lua_pushstring(L, buf);
    return 1;
}
int engine_stats(lua_State* L) {
    Engine& e = E(L);
    lua_createtable(L, 0, 6);
    lua_pushinteger(L, e.world().alive()); lua_setfield(L, -2, "entities");
    lua_pushinteger(L, e.physics().stats.awake); lua_setfield(L, -2, "awake_bodies");
    lua_pushinteger(L, lua_Integer(e.lua().bytes())); lua_setfield(L, -2, "lua_bytes");
    lua_pushinteger(L, e.mixer().active_voices()); lua_setfield(L, -2, "voices");
    lua_pushinteger(L, e.renderer().last_frame_stats().batches); lua_setfield(L, -2, "batches");
    lua_pushnumber(L, e.render_clock().frame_ms); lua_setfield(L, -2, "frame_ms");
    return 1;
}
int log_fn(lua_State* L) {
    std::string out;
    for (int i = 1; i <= lua_gettop(L); i++) { if (i > 1) out += " "; out += luaL_tolstring(L, i, nullptr); lua_pop(L, 1); }
    E(L).script_log(out);
    return 0;
}
int save_state_fn(lua_State* L) { E(L).request_save(luaL_optstring(L, 1, "")); lua_pushboolean(L, 1); return 1; }
int load_state_fn(lua_State* L) { E(L).request_load(luaL_optstring(L, 1, "")); lua_pushboolean(L, 1); return 1; }

// ------------------------------------------------------------------------------ gc
int gc_step(lua_State* L) { E(L).lua().gc_step(); return 0; }
int gc_budget(lua_State* L) { if (!lua_isnoneornil(L, 1)) E(L).lua().gc_budget(int(luaL_checkinteger(L, 1))); lua_pushinteger(L, E(L).lua().gc_budget_kb()); return 1; }
int gc_pause(lua_State* L) { E(L).lua().gc_pause(); return 0; }
int gc_resume(lua_State* L) { E(L).lua().gc_resume(); return 0; }
int gc_collect(lua_State* L) { E(L).lua().gc_collect(); return 0; }
int gc_bytes(lua_State* L) { lua_pushinteger(L, lua_Integer(E(L).lua().bytes())); return 1; }

// ------------------------------------------------------------------------------ animation / map
int anim_define(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    AnimationClip c;
    lua_getfield(L, 1, "name"); c.name = luaL_checkstring(L, -1); lua_pop(L, 1);
    lua_getfield(L, 1, "mode");
    std::string mode = lua_isstring(L, -1) ? lua_tostring(L, -1) : "loop";
    lua_pop(L, 1);
    c.mode = mode == "once" ? PlayMode::Once : mode == "pingpong" ? PlayMode::PingPong : PlayMode::Loop;
    lua_getfield(L, 1, "texture");
    const TextureAsset* tex = lua_isstring(L, -1) ? E(L).texture(lua_tostring(L, -1)) : nullptr;
    lua_pop(L, 1);
    lua_getfield(L, 1, "frames");
    if (lua_istable(L, -1)) {
        if (!tex) return luaL_error(L, "animation '%s' has frames but no texture", c.name.c_str());
        for (int i = 1; i <= int(lua_rawlen(L, -1)); i++) {
            lua_rawgeti(L, -1, i);
            lua_rawgeti(L, -1, 1); lua_rawgeti(L, -2, 2);
            const char* region = luaL_checkstring(L, -2);
            int r = tex->atlas.find(region);
            if (r < 0) return luaL_error(L, "animation '%s': unknown atlas region '%s'", c.name.c_str(), region);
            c.frames.push_back({uint32_t(r), float(luaL_checknumber(L, -1))});
            lua_pop(L, 3);
        }
    }
    lua_pop(L, 1);
    lua_getfield(L, 1, "tracks");
    if (lua_istable(L, -1)) {
        lua_pushnil(L);
        while (lua_next(L, -2)) {
            Track t;
            t.property = luaL_checkstring(L, -2);
            for (int i = 1; i <= int(lua_rawlen(L, -1)); i++) {
                lua_rawgeti(L, -1, i);
                lua_rawgeti(L, -1, 1); lua_rawgeti(L, -2, 2);
                t.keys.push_back({float(luaL_checknumber(L, -2)), float(luaL_checknumber(L, -1))});
                lua_pop(L, 3);
            }
            std::sort(t.keys.begin(), t.keys.end(), [](const Keyframe& a, const Keyframe& b) { return a.time < b.time; });
            c.tracks.push_back(t);
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
    E(L).animation().add_clip(c);
    return 0;
}
int map_load(lua_State* L) {
    AssetHandle h = scene_asset(L, AssetType::Map, luaL_checkstring(L, 1));
    const TileMap& m = E(L).assets().get(h)->map;
    lua_createtable(L, 0, 4);
    lua_pushinteger(L, m.width); lua_setfield(L, -2, "width");
    lua_pushinteger(L, m.height); lua_setfield(L, -2, "height");
    lua_pushnumber(L, m.tile_size); lua_setfield(L, -2, "tile_size");
    lua_createtable(L, m.height, 0);
    for (int i = 0; i < m.height; i++) { lua_pushstring(L, m.rows[size_t(i)].c_str()); lua_rawseti(L, -2, i + 1); }
    lua_setfield(L, -2, "rows");
    return 1;
}

// ------------------------------------------------------------------------------ buffer / kernel
LuaBuffer* check_buffer(lua_State* L, int i) {
    auto* b = static_cast<LuaBuffer*>(luaL_checkudata(L, i, kBufferMeta));
    if (!E(L).compute().buffer_valid(BufferHandle{b->bits})) luaL_error(L, "stale buffer handle");
    return b;
}
int buffer_create(lua_State* L) {
    lua_Integer n = luaL_checkinteger(L, 1);
    if (n <= 0 || n > (1 << 24)) return luaL_error(L, "buffer size out of range");
    bool is_int = std::strcmp(luaL_optstring(L, 2, "float"), "int") == 0;
    BufferHandle h = E(L).compute().buffer_create(uint32_t(n) * 4);
    if (!h) return luaL_error(L, "out of kernel memory");
    if (Scene* s = E(L).current_scene()) s->buffers.push_back(h);
    auto* b = static_cast<LuaBuffer*>(lua_newuserdatauv(L, sizeof(LuaBuffer), 0));
    *b = {h.bits, is_int};
    luaL_setmetatable(L, kBufferMeta);
    return 1;
}
int buffer_upload(lua_State* L) {
    LuaBuffer* b = check_buffer(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    uint32_t offset = uint32_t(luaL_optinteger(L, 3, 0));
    uint32_t n = uint32_t(lua_rawlen(L, 2));
    uint32_t cap = E(L).compute().buffer_size(BufferHandle{b->bits}) / 4;
    if (offset + n > cap) return luaL_error(L, "upload of %d elements at %d overflows buffer of %d", int(n), int(offset), int(cap));
    std::vector<uint32_t> tmp(n);
    for (uint32_t i = 0; i < n; i++) {
        lua_rawgeti(L, 2, lua_Integer(i) + 1);
        if (b->is_int) { int32_t v = int32_t(lua_tointeger(L, -1)); std::memcpy(&tmp[i], &v, 4); }
        else { float v = float(lua_tonumber(L, -1)); std::memcpy(&tmp[i], &v, 4); }
        lua_pop(L, 1);
    }
    E(L).compute().buffer_upload(BufferHandle{b->bits}, tmp.data(), n * 4, offset * 4);
    return 0;
}
int buffer_download(lua_State* L) {
    LuaBuffer* b = check_buffer(L, 1);
    uint32_t cap = E(L).compute().buffer_size(BufferHandle{b->bits}) / 4;
    uint32_t n = uint32_t(luaL_optinteger(L, 2, cap));
    if (n > cap) n = cap;
    std::vector<uint32_t> tmp(n);
    E(L).compute().buffer_download(BufferHandle{b->bits}, tmp.data(), n * 4);
    lua_createtable(L, int(n), 0);
    for (uint32_t i = 0; i < n; i++) {
        if (b->is_int) { int32_t v; std::memcpy(&v, &tmp[i], 4); lua_pushinteger(L, v); }
        else { float v; std::memcpy(&v, &tmp[i], 4); lua_pushnumber(L, v); }
        lua_rawseti(L, -2, lua_Integer(i) + 1);
    }
    return 1;
}
int buffer_destroy(lua_State* L) {
    LuaBuffer* b = check_buffer(L, 1);
    E(L).compute().buffer_destroy(BufferHandle{b->bits});
    if (Scene* s = E(L).current_scene()) s->buffers.erase(std::remove(s->buffers.begin(), s->buffers.end(), BufferHandle{b->bits}), s->buffers.end());
    return 0;
}
int buffer_size(lua_State* L) { lua_pushinteger(L, E(L).compute().buffer_size(BufferHandle{check_buffer(L, 1)->bits}) / 4); return 1; }

int kernel_compile(lua_State* L) {
    std::string src = luaL_checkstring(L, 1);
    if (src.find("__global__") == std::string::npos) {  // an asset name, e.g. "kernels/particles"
        AssetHandle h = scene_asset(L, AssetType::Kernel, src);
        src = E(L).assets().get(h)->text;
    }
    kernel::CompiledKernel k = E(L).compute().compile(src);
    lua_createtable(L, 0, 6);
    lua_pushboolean(L, k.ok); lua_setfield(L, -2, "ok");
    lua_pushstring(L, k.error.c_str()); lua_setfield(L, -2, "error");
    lua_pushstring(L, k.name.c_str()); lua_setfield(L, -2, "name");
    lua_pushstring(L, k.ir_text.c_str()); lua_setfield(L, -2, "ir");
    lua_pushinteger(L, lua_Integer(k.wasm.size())); lua_setfield(L, -2, "wasm_bytes");
    lua_pushstring(L, src.c_str()); lua_setfield(L, -2, "source");
    return 1;
}
int kernel_load(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_getfield(L, 1, "source");
    std::string src = luaL_checkstring(L, -1);
    lua_pop(L, 1);
    std::string err;
    KernelHandle h = E(L).compute().compile_and_load(src, err);
    if (!h) return luaL_error(L, "kernel load failed: %s", err.c_str());
    if (Scene* s = E(L).current_scene()) s->kernels.push_back(h);
    *static_cast<uint32_t*>(lua_newuserdatauv(L, sizeof(uint32_t), 0)) = h.bits;
    luaL_setmetatable(L, kKernelMeta);
    return 1;
}
int kernel_dispatch(lua_State* L) {
    KernelHandle k{*static_cast<uint32_t*>(luaL_checkudata(L, 1, kKernelMeta))};
    if (!E(L).compute().kernel_valid(k)) return luaL_error(L, "stale kernel handle");
    uint32_t grid = uint32_t(luaL_checkinteger(L, 2)), block = uint32_t(luaL_checkinteger(L, 3));
    std::vector<KernelArg> args;
    for (int i = 4; i <= lua_gettop(L); i++) {
        if (luaL_testudata(L, i, kBufferMeta)) args.push_back(KernelArg::buf(BufferHandle{check_buffer(L, i)->bits}));
        else if (lua_isinteger(L, i)) args.push_back(KernelArg::integer(int32_t(lua_tointeger(L, i))));
        else args.push_back(KernelArg::real(float(luaL_checknumber(L, i))));
    }
    DispatchResult r = E(L).compute().dispatch(k, grid, block, args);
    if (!r.ok) return luaL_error(L, "kernel dispatch failed: %s", r.error.c_str());
    lua_pushinteger(L, lua_Integer(r.instructions));
    return 1;
}

void make_lib(lua_State* L, Engine& e, const char* global, const luaL_Reg* fns) {
    lua_newtable(L);
    lua_pushlightuserdata(L, &e);
    luaL_setfuncs(L, fns, 1);
    lua_setglobal(L, global);
}
}  // namespace

void register_bindings(Engine& engine) {
    lua_State* L = engine.lua().L();

    luaL_newmetatable(L, kEntityMeta);
    static const luaL_Reg entity_methods[] = {
        {"set_position", ent_set_position}, {"position", ent_position}, {"set_velocity", ent_set_velocity}, {"velocity", ent_velocity},
        {"apply_impulse", ent_apply_impulse}, {"set_rotation", ent_set_rotation}, {"rotation", ent_rotation},
        {"set_animation", ent_set_animation}, {"animation", ent_animation}, {"set_flip", ent_set_flip}, {"set_visible", ent_set_visible},
        {"set_tint", ent_set_tint}, {"name", ent_name}, {"valid", ent_valid}, {"id", ent_id}, {"destroy", ent_destroy}, {nullptr, nullptr}};
    lua_newtable(L);
    lua_pushlightuserdata(L, &engine);
    luaL_setfuncs(L, entity_methods, 1);
    lua_setfield(L, -2, "__index");
    lua_pushlightuserdata(L, &engine); lua_pushcclosure(L, ent_eq, 1); lua_setfield(L, -2, "__eq");
    lua_pushlightuserdata(L, &engine); lua_pushcclosure(L, ent_tostring, 1); lua_setfield(L, -2, "__tostring");
    lua_pop(L, 1);
    luaL_newmetatable(L, kBufferMeta); lua_pop(L, 1);
    luaL_newmetatable(L, kKernelMeta); lua_pop(L, 1);

    static const luaL_Reg world_fns[] = {{"spawn", world_spawn}, {"destroy", world_destroy}, {"find", world_find}, {"count", world_count}, {nullptr, nullptr}};
    static const luaL_Reg physics_fns[] = {{"apply_force", phys_apply_force}, {"apply_impulse", phys_apply_impulse}, {"raycast", phys_raycast},
                                           {"set_gravity", phys_set_gravity}, {"add_static_box", phys_add_static_box}, {"is_awake", phys_is_awake}, {nullptr, nullptr}};
    static const luaL_Reg audio_fns[] = {{"play", audio_play}, {"stream", audio_stream}, {"stop", audio_stop}, {"pause", audio_pause}, {"resume", audio_resume},
                                         {"fade", audio_fade}, {"playing", audio_playing}, {"set_bus_volume", audio_set_bus_volume}, {nullptr, nullptr}};
    static const luaL_Reg input_fns[] = {{"is_pressed", in_pressed}, {"is_held", in_held}, {"is_released", in_released}, {"axis", in_axis},
                                         {"buffered", in_buffered}, {"consume", in_consume}, {"pointer", in_pointer}, {"bind", in_bind}, {"bind_axis", in_bind_axis}, {nullptr, nullptr}};
    static const luaL_Reg camera_fns[] = {{"set", cam_set}, {"get", cam_get}, {nullptr, nullptr}};
    static const luaL_Reg ui_fns[] = {{"text", ui_text}, {"image", ui_image}, {"rect", ui_rect}, {"width", ui_width}, {"screen", ui_screen}, {nullptr, nullptr}};
    static const luaL_Reg timer_fns[] = {{"after", timer_after}, {"every", timer_every}, {nullptr, nullptr}};
    static const luaL_Reg scene_fns[] = {{"transition", scene_transition}, {"current", scene_current}, {"state", scene_state}, {nullptr, nullptr}};
    static const luaL_Reg engine_fns[] = {{"tick", engine_tick}, {"time", engine_time}, {"quit", engine_quit}, {"state_hash", engine_hash}, {"stats", engine_stats}, {nullptr, nullptr}};
    static const luaL_Reg gc_fns[] = {{"step", gc_step}, {"budget", gc_budget}, {"pause", gc_pause}, {"resume", gc_resume}, {"collect", gc_collect}, {"bytes", gc_bytes}, {nullptr, nullptr}};
    static const luaL_Reg anim_fns[] = {{"define", anim_define}, {nullptr, nullptr}};
    static const luaL_Reg map_fns[] = {{"load", map_load}, {nullptr, nullptr}};
    static const luaL_Reg buffer_fns[] = {{"create", buffer_create}, {"upload", buffer_upload}, {"download", buffer_download}, {"destroy", buffer_destroy}, {"size", buffer_size}, {nullptr, nullptr}};
    static const luaL_Reg kernel_fns[] = {{"compile", kernel_compile}, {"load", kernel_load}, {"dispatch", kernel_dispatch}, {nullptr, nullptr}};
    make_lib(L, engine, "world", world_fns);
    make_lib(L, engine, "physics", physics_fns);
    make_lib(L, engine, "audio", audio_fns);
    make_lib(L, engine, "input", input_fns);
    make_lib(L, engine, "camera", camera_fns);
    make_lib(L, engine, "ui", ui_fns);
    make_lib(L, engine, "timer", timer_fns);
    make_lib(L, engine, "scene", scene_fns);
    make_lib(L, engine, "engine", engine_fns);
    make_lib(L, engine, "gc", gc_fns);
    make_lib(L, engine, "animation", anim_fns);
    make_lib(L, engine, "map", map_fns);
    make_lib(L, engine, "buffer", buffer_fns);
    make_lib(L, engine, "kernel", kernel_fns);
    // gc_step() etc. also as globals, matching the engine-level API names.
    for (auto* r = gc_fns; r->name; r++) {
        lua_pushlightuserdata(L, &engine);
        lua_pushcclosure(L, r->func, 1);
        lua_setglobal(L, (std::string("gc_") + r->name).c_str());
    }
    lua_pushlightuserdata(L, &engine); lua_pushcclosure(L, log_fn, 1); lua_setglobal(L, "log");
    lua_pushlightuserdata(L, &engine); lua_pushcclosure(L, save_state_fn, 1); lua_setglobal(L, "save_state");
    lua_pushlightuserdata(L, &engine); lua_pushcclosure(L, load_state_fn, 1); lua_setglobal(L, "load_state");
    lua_newtable(L);
    lua_setglobal(L, "game");  // persistent gameplay state, serialized in save states
}

}  // namespace unify
