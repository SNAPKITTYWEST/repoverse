#include "engine.h"
#include "../serialization/stream.h"
#include <lua.h>
#include <lauxlib.h>
#include <algorithm>
#include <chrono>
#include <cstring>

namespace unify {

const char* stage_name(Stage s) {
    static const char* names[] = {"INPUT", "INPUT BUFFER", "FIXED SIMULATION", "PHYSICS", "GAMEPLAY / LUA", "ANIMATION", "AUDIO", "RENDER PREPARATION", "RENDER", "PRESENT"};
    return s < Stage::Count ? names[size_t(s)] : "?";
}

Engine::Engine(Backend& backend, EngineConfig config)
    : backend_(backend), config_(std::move(config)), mixer_(44100, 64), target_(config_.width, config_.height), rng_(config_.seed) {
    sim_.ticks_per_second = config_.tick_rate;
    assets_ = std::make_unique<AssetManager>(backend_.render_device(), config_.asset_root);
    renderer_ = std::make_unique<Renderer2D>(backend_.render_device());
    lua_ = std::make_unique<LuaVM>(config_.lua_memory_limit);
    lua_->gc_budget(config_.lua_gc_budget_kb);
    // Entity destruction releases the subsystem resources it owns.
    world_.add_destroy_hook([this](EntityHandle e) {
        if (BodyC* b = world_.bodies.get(e)) physics_.destroy_body(PhysicsBodyHandle{b->body});
        if (AnimatorC* a = world_.animators.get(e)) anim_.destroy_player(a->player);
        if (SpriteC* s = world_.sprites.get(e)) (void)s;  // textures are owned by scenes, not entities
    });
    physics_.gravity = {0, -30};
    trace_.reserve(64);
    events_scratch_.reserve(512);
}

Engine::~Engine() {
    streamer_.stop();
    backend_.audio_device().stop();
    mixer_.stop_all();
    while (!scenes_.empty()) { unload_scene(scenes_.back()); scenes_.pop_back(); }
}

bool Engine::boot(std::string& error) {
    if (!backend_.init(config_, error)) return false;
    font_ = BitmapFont::build();
    font_.texture = backend_.render_device().create_texture(font_.image).bits;
    if (!backend_.audio_device().start(mixer_)) UNIFY_LOG_WARN("AUDIO", "audio device '%s' failed to start; continuing silent", backend_.audio_device().name());
    streamer_.start(mixer_);
    lua_->sandbox(&rng_);
    register_bindings(*this);
    UNIFY_LOG_INFO("BOOT", "%s on %s backend, %dx%d, %u Hz simulation", kEngineVersion, backend_.name(), config_.width, config_.height, config_.tick_rate);
    if (!transition(config_.boot_scene, error)) return false;
    sim_origin_ns_ = backend_.now_ns();
    booted_ = true;
    return true;
}

void Engine::script_log(const std::string& s) {
    script_log_.push_back(s);
    if (script_log_.size() > 256) script_log_.erase(script_log_.begin());
    UNIFY_LOG_INFO("LUA", "%s", s.c_str());
}

void Engine::report_script_error(const std::string& where) {
    std::string msg = where + ": " + lua_->last_error();
    script_log("ERROR " + msg);
    UNIFY_LOG_ERROR("LUA", "%s", msg.c_str());
}

// ------------------------------------------------------------------------------ scheduler

void Engine::frame() {
    auto t0 = std::chrono::steady_clock::now();
    trace_.clear();
    mark(Stage::Input);
    backend_.poll(input_, quit_);
    const uint64_t now = backend_.now_ns();
    const uint64_t dt_ns = 1000000000ull / sim_.ticks_per_second;
    uint32_t ran = 0;
    while (sim_origin_ns_ + (sim_.tick + 1) * 1000000000ull / sim_.ticks_per_second <= now && ran < uint32_t(config_.max_ticks_per_frame) && !quit_) {
        tick();
        ran++;
    }
    if (ran == uint32_t(config_.max_ticks_per_frame)) {
        // Too far behind (breakpoint, stall): drop the backlog instead of spiralling.
        sim_origin_ns_ = now - sim_.tick * 1000000000ull / sim_.ticks_per_second;
    }
    stats_.ticks_this_frame = ran;
    uint64_t tick_start = sim_origin_ns_ + sim_.tick * 1000000000ull / sim_.ticks_per_second;
    float alpha = now > tick_start ? clampf(float(double(now - tick_start) / double(dt_ns)), 0, 1) : 0;
    render(alpha);
    render_clock_.frame_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

int Engine::run() {
    while (!quit_) frame();
    return 0;
}

void Engine::tick_impl(const InputFrame* replay) {
    if (!trace_.empty() && trace_.back() == Stage::Present) trace_.clear();
    uint64_t heap0 = HeapStats::allocations.load();
    auto stage_start = std::chrono::steady_clock::now();
    auto lap = [&](Stage s) {
        auto t = std::chrono::steady_clock::now();
        stats_.stage_ms[size_t(s)] = std::chrono::duration<double, std::milli>(t - stage_start).count();
        stage_start = t;
    };
    const float dt = sim_.dtf();

    mark(Stage::InputBuffer);
    if (replay) input_.apply_frame(*replay);
    else input_.consume_tick(sim_.tick, sim_origin_ns_ + sim_.tick_end_ns(sim_.tick));
    lap(Stage::InputBuffer);

    mark(Stage::FixedSimulation);
    for (size_t i = 0; i < world_.transforms.size(); i++) world_.transforms.at(i).previous = world_.transforms.at(i).current;
    lap(Stage::FixedSimulation);

    mark(Stage::Physics);
    physics_.step(dt);
    sync_transforms_from_physics();
    lap(Stage::Physics);

    mark(Stage::Gameplay);
    dispatch_physics_events();
    fire_timers();
    lua_pushnumber(lua_->L(), dt);
    if (!call_scene("update", 1)) report_script_error("scene update");
    if (!pending_transition_.empty()) {
        std::string target = pending_transition_, err;
        pending_transition_.clear();
        if (!transition(target, err)) UNIFY_LOG_ERROR("SCENE", "transition to '%s' failed: %s", target.c_str(), err.c_str());
    }
    lua_->gc_step();  // bounded incremental GC every tick: no unpredictable full-collection stalls
    lap(Stage::Gameplay);

    mark(Stage::Animation);
    anim_.advance(dt);
    lap(Stage::Animation);

    mark(Stage::Audio);
    for (auto& s : scenes_)  // forget voices that finished on the audio thread
        s.voices.erase(std::remove_if(s.voices.begin(), s.voices.end(), [&](AudioHandle h) { return !mixer_.alive(h); }), s.voices.end());
    lap(Stage::Audio);

    sim_.tick++;
    stats_.heap_allocs_last_tick = HeapStats::allocations.load() - heap0;
    stats_.lua_bytes = lua_->bytes();
    process_pending_state_ops();
}

void Engine::render(float alpha) {
    auto t0 = std::chrono::steady_clock::now();
    mark(Stage::RenderPrep);
    render_clock_.alpha = alpha;
    render_clock_.frame++;
    render_clock_.seconds = double(backend_.now_ns()) * 1e-9;
    frame_alloc_.reset();
    renderer_->begin(target_, camera_, {});
    for (size_t i = 0; i < world_.sprites.size(); i++) {
        const SpriteC& s = world_.sprites.at(i);
        if (!s.visible) continue;
        EntityHandle e = world_.sprites.owner(i);
        const TransformC* t = world_.transforms.get(e);
        if (!t) continue;
        // Interpolate between the last two simulation states: smooth at any frame rate.
        Vec2 pos = lerp(t->previous.position, t->current.position, alpha);
        float rot = t->previous.rotation + (t->current.rotation - t->previous.rotation) * alpha;
        const AssetRecord* tex = assets_->get(AssetHandle{s.texture});
        uint32_t region = s.atlas_region;
        Vec2 size = s.size;
        Color tint = s.tint;
        if (const AnimatorC* a = world_.animators.get(e)) {
            if (uint32_t r = anim_.sprite_region(a->player)) region = r;
            size.x *= anim_.value(a->player, "scale_x", 1);
            size.y *= anim_.value(a->player, "scale_y", 1);
            rot += anim_.value(a->player, "rotation", 0);
            tint.a = uint8_t(clampf(float(tint.a) * anim_.value(a->player, "alpha", 1), 0, 255));
        }
        RectF src{0, 0, 1, 1};
        uint32_t device_tex = 0;
        if (tex && tex->type == AssetType::Texture) {
            device_tex = tex->texture.texture.bits;
            const auto& regs = tex->texture.atlas.regions;
            RectI rr = regs[region < regs.size() ? region : 0].rect;
            src = {float(rr.x), float(rr.y), float(rr.w), float(rr.h)};
        }
        renderer_->sprite(device_tex, src, pos, size, rot, tint, s.layer, s.depth, BlendMode::Alpha, s.flip_x, s.pivot);
    }
    rendering_ = true;  // ui:* calls are only valid here
    if (!call_scene("render")) report_script_error("scene render");
    rendering_ = false;
    stats_.stage_ms[size_t(Stage::RenderPrep)] = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

    mark(Stage::Render);
    auto t1 = std::chrono::steady_clock::now();
    renderer_->end();
    stats_.stage_ms[size_t(Stage::Render)] = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count();

    mark(Stage::Present);
    backend_.present(target_);
}

void Engine::sync_transforms_from_physics() {
    for (size_t i = 0; i < world_.bodies.size(); i++) {
        EntityHandle e = world_.bodies.owner(i);
        const Body* b = physics_.get(PhysicsBodyHandle{world_.bodies.at(i).body});
        TransformC* t = world_.transforms.get(e);
        if (!b || !t) continue;
        // Physics has already contained its own failures; only finite poses reach the ECS.
        if (!finite(b->position) || !std::isfinite(b->angle)) continue;
        t->current.position = b->position;
        t->current.rotation = b->angle;
    }
}

static void push_entity(lua_State* L, uint32_t bits) {
    if (!bits) { lua_pushnil(L); return; }
    *static_cast<uint32_t*>(lua_newuserdatauv(L, sizeof(uint32_t), 0)) = bits;
    luaL_setmetatable(L, kEntityMeta);
}

void Engine::dispatch_physics_events() {
    static const char* names[] = {"begin", "persist", "end", "trigger_begin", "trigger_end"};
    // Copy first: gameplay may destroy bodies from inside a callback, which appends End
    // events to the physics event list and would invalidate a live iterator.
    events_scratch_ = physics_.events();
    for (const ContactEvent& ev : events_scratch_) {
        if (ev.type == ContactEventType::Persist) continue;  // high-volume; gameplay polls contacts via raycasts instead
        lua_State* L = lua_->L();
        push_entity(L, world_.valid(EntityHandle{ev.user_a}) ? ev.user_a : 0);
        push_entity(L, world_.valid(EntityHandle{ev.user_b}) ? ev.user_b : 0);
        lua_pushstring(L, names[int(ev.type)]);
        lua_pushnumber(L, ev.normal.x);
        lua_pushnumber(L, ev.normal.y);
        lua_pushnumber(L, ev.impulse);
        if (!call_scene("on_contact", 6)) report_script_error("scene on_contact");
    }
}

void Engine::add_timer(double seconds, bool repeat, const std::string& name) {
    uint64_t ticks = std::max<uint64_t>(1, uint64_t(seconds * sim_.ticks_per_second + 0.5));
    timers_.push_back({sim_.tick + ticks, repeat ? ticks : 0, name, current_scene() ? current_scene()->id : uint16_t(0)});
}

void Engine::fire_timers() {
    for (size_t i = 0; i < timers_.size();) {
        Timer& t = timers_[i];
        if (t.fire_tick > sim_.tick) { i++; continue; }
        std::string name = t.name;
        if (t.interval_ticks) { t.fire_tick += t.interval_ticks; i++; }
        else timers_.erase(timers_.begin() + long(i));
        lua_pushstring(lua_->L(), name.c_str());
        if (!call_scene("on_timer", 1)) report_script_error("scene on_timer");
    }
}

// ------------------------------------------------------------------------------ scenes

bool Engine::call_scene(const char* fn, int nargs) {
    lua_State* L = lua_->L();
    Scene* s = current_scene();
    if (!s || s->lua_ref < 0) { lua_pop(L, nargs); return true; }
    lua_rawgeti(L, LUA_REGISTRYINDEX, s->lua_ref);
    lua_getfield(L, -1, fn);
    if (!lua_isfunction(L, -1)) { lua_pop(L, 2 + nargs); return true; }
    lua_insert(L, -(nargs + 2));      // fn below args
    lua_insert(L, -(nargs + 1));      // scene table as first arg (method call)
    if (lua_pcall(L, nargs + 1, 0, 0) != LUA_OK) {
        const char* msg = lua_tostring(L, -1);
        lua_->set_error(msg ? msg : "(non-string error)");
        lua_pop(L, 1);
        return false;
    }
    return true;
}

bool Engine::load_scene_script(Scene& s, std::string& error) {
    s.script = assets_->load(AssetType::Script, "scenes/" + s.name);
    if (!s.script) { error = "scene script 'scenes/" + s.name + "' not found"; return false; }
    const AssetRecord* rec = assets_->get(s.script);
    if (!lua_->run(rec->text, rec->path, 1)) { error = lua_->last_error(); return false; }
    lua_State* L = lua_->L();
    if (!lua_istable(L, -1)) { lua_pop(L, 1); error = "scene script must return a table"; return false; }
    lua_pushstring(L, s.name.c_str());
    lua_setfield(L, -2, "name");
    s.lua_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    return true;
}

void Engine::unload_scene(Scene& s) {
    // Entities owned by this scene (persistent ones have scene 0 and survive).
    std::vector<EntityHandle> doomed;
    world_.each_entity([&](EntityHandle e) { if (world_.info(e)->scene == s.id) doomed.push_back(e); });
    for (EntityHandle e : doomed) world_.destroy(e);
    for (AudioHandle v : s.voices) mixer_.stop(v);
    for (BufferHandle b : s.buffers) compute_.buffer_destroy(b);
    for (KernelHandle k : s.kernels) compute_.unload(k);
    for (AssetHandle a : s.assets) assets_->release(a);
    if (s.script) assets_->release(s.script);
    if (s.lua_ref >= 0) luaL_unref(lua_->L(), LUA_REGISTRYINDEX, s.lua_ref);
    timers_.erase(std::remove_if(timers_.begin(), timers_.end(), [&](const Timer& t) { return t.scene == s.id; }), timers_.end());
    s = Scene{};
}

bool Engine::transition(const std::string& name, std::string& error) {
    if (!scenes_.empty()) {
        call_scene("unload");
        unload_scene(scenes_.back());
        scenes_.pop_back();
    }
    return push_scene(name, error);
}

bool Engine::push_scene(const std::string& name, std::string& error) {
    Scene s;
    s.id = next_scene_id_++;
    s.name = name;
    s.voices.reserve(64);  // playing a sound never grows this list mid-tick
    scenes_.push_back(s);
    if (!load_scene_script(scenes_.back(), error)) { unload_scene(scenes_.back()); scenes_.pop_back(); return false; }
    lua_->gc_collect();  // scene loads are the one place a full collection is allowed
    // init(): code-level setup (bindings, animation clips); also runs after a restore.
    // load(): creates the scene's world content; skipped on restore (the save provides it).
    if (!call_scene("init") || !call_scene("load")) {
        // A scene that fails to load is removed completely: no half-built world is left running.
        error = lua_->last_error();
        report_script_error("scene load");
        unload_scene(scenes_.back());
        scenes_.pop_back();
        return false;
    }
    UNIFY_LOG_INFO("SCENE", "loaded '%s' (id %u)", name.c_str(), scenes_.back().id);
    return true;
}

bool Engine::pop_scene(std::string& error) {
    if (scenes_.size() <= 1) { error = "cannot pop the last scene"; return false; }
    call_scene("unload");
    unload_scene(scenes_.back());
    scenes_.pop_back();
    return true;
}

ResourceCounts Engine::resources() const {
    uint32_t refs = 0;
    for (auto& s : scenes_) refs += s.lua_ref >= 0;
    return {world_.alive(), physics_.body_count(), uint32_t(const_cast<AnimationSystem&>(anim_).live_players()), mixer_.active_voices(),
            compute_.live_buffers(), compute_.live_kernels(), assets_->live(), const_cast<Backend&>(backend_).render_device().live_textures(), refs};
}

// ------------------------------------------------------------------------------ prefabs

const TextureAsset* Engine::texture(const std::string& name) {
    AssetHandle h = assets_->find(name);
    Scene* s = current_scene();
    if (!h) {
        h = assets_->load(AssetType::Texture, name);
        if (!h) return nullptr;
        if (s) s->assets.push_back(h); else assets_->acquire(h);
    } else if (s && std::find(s->assets.begin(), s->assets.end(), h) == s->assets.end()) {
        assets_->acquire(h);
        s->assets.push_back(h);
    }
    return &assets_->get(h)->texture;
}

static float field_num(lua_State* L, int t, const char* k, float def) {
    lua_getfield(L, t, k);
    float v = lua_isnumber(L, -1) ? float(lua_tonumber(L, -1)) : def;
    lua_pop(L, 1);
    return v;
}
static std::string field_str(lua_State* L, int t, const char* k, const std::string& def = {}) {
    lua_getfield(L, t, k);
    std::string v = lua_isstring(L, -1) ? lua_tostring(L, -1) : def;
    lua_pop(L, 1);
    return v;
}
static bool field_bool(lua_State* L, int t, const char* k, bool def) {
    lua_getfield(L, t, k);
    bool v = lua_isboolean(L, -1) ? lua_toboolean(L, -1) : def;
    lua_pop(L, 1);
    return v;
}

const PrefabDef* Engine::prefab(const std::string& name, std::string& error) {
    auto it = prefabs_.find(name);
    if (it != prefabs_.end()) return &it->second;
    AssetHandle h = assets_->load(AssetType::Prefab, name);
    if (!h) { error = "prefab '" + name + "' not found"; return nullptr; }
    std::string text = assets_->get(h)->text, path = assets_->get(h)->path;
    assets_->release(h);  // parsed into a PrefabDef; the source text is not kept
    if (!lua_->run(text, path, 1)) { error = lua_->last_error(); return nullptr; }
    lua_State* L = lua_->L();
    int t = lua_gettop(L);
    if (!lua_istable(L, t)) { lua_pop(L, 1); error = "prefab must return a table"; return nullptr; }
    PrefabDef d;
    d.name = field_str(L, t, "name", name);
    d.animation = field_str(L, t, "animation");
    d.tags = uint32_t(field_num(L, t, "tags", 0));
    lua_getfield(L, t, "sprite");
    if (lua_istable(L, -1)) {
        int st = lua_gettop(L);
        d.has_sprite = true;
        d.texture = field_str(L, st, "texture");
        d.region = field_str(L, st, "region");
        d.sprite_size = {field_num(L, st, "w", 1), field_num(L, st, "h", 1)};
        d.layer = int16_t(field_num(L, st, "layer", 0));
        d.depth = field_num(L, st, "depth", 0);
        lua_getfield(L, st, "tint");
        if (lua_istable(L, -1)) {
            int tt = lua_gettop(L);
            d.tint = {uint8_t(field_num(L, tt, "r", 255)), uint8_t(field_num(L, tt, "g", 255)), uint8_t(field_num(L, tt, "b", 255)), uint8_t(field_num(L, tt, "a", 255))};
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    lua_getfield(L, t, "body");
    if (lua_istable(L, -1)) {
        int bt = lua_gettop(L);
        d.has_body = true;
        BodyDef& b = d.body;
        std::string type = field_str(L, bt, "type", "dynamic");
        b.type = type == "static" ? BodyType::Static : type == "kinematic" ? BodyType::Kinematic : BodyType::Dynamic;
        std::string shape = field_str(L, bt, "shape", "aabb");
        float w = field_num(L, bt, "w", 1), hh = field_num(L, bt, "h", 1);
        if (shape == "circle") b.shape = Shape::circle(field_num(L, bt, "r", 0.5f));
        else if (shape == "box") b.shape = Shape::box(w / 2, hh / 2);
        else if (shape == "polygon") {
            Vec2 pts[phys::kMaxPolygonVertices];
            int n = 0;
            lua_getfield(L, bt, "points");
            if (lua_istable(L, -1)) {
                for (int i = 1; i <= int(lua_rawlen(L, -1)) && n < phys::kMaxPolygonVertices; i++) {
                    lua_rawgeti(L, -1, i);
                    int pt = lua_gettop(L);
                    lua_rawgeti(L, pt, 1); lua_rawgeti(L, pt, 2);
                    pts[n++] = {float(lua_tonumber(L, -2)), float(lua_tonumber(L, -1))};
                    lua_pop(L, 3);
                }
            }
            lua_pop(L, 1);
            b.shape = Shape::polygon(pts, n);
        } else b.shape = Shape::aabb(w / 2, hh / 2);
        b.density = field_num(L, bt, "density", 1);
        b.mass = field_num(L, bt, "mass", 0);
        b.friction = field_num(L, bt, "friction", 0.6f);
        b.restitution = field_num(L, bt, "restitution", 0);
        b.gravity_scale = field_num(L, bt, "gravity_scale", 1);
        b.linear_damping = field_num(L, bt, "linear_damping", 0);
        b.fixed_rotation = field_bool(L, bt, "fixed_rotation", false);
        b.bullet = field_bool(L, bt, "bullet", false);
        b.sensor = field_bool(L, bt, "sensor", false);
        b.can_sleep = field_bool(L, bt, "can_sleep", true);
        b.category = uint32_t(field_num(L, bt, "category", 1));
        b.mask = uint32_t(field_num(L, bt, "mask", 65535));
    }
    lua_pop(L, 2);
    return &(prefabs_[name] = d);
}

EntityHandle Engine::instantiate(const std::string& name, Vec2 pos, bool persistent, std::string& error) {
    const PrefabDef* d = prefab(name, error);
    if (!d) return {};
    Scene* s = current_scene();
    EntityHandle e = world_.create(d->name, persistent || !s ? 0 : s->id);
    world_.info(e)->prefab = name;
    world_.info(e)->tags = d->tags;
    TransformC* t = world_.transforms.get(e);
    t->current.position = t->previous.position = pos;
    if (d->has_sprite) {
        SpriteC sp;
        if (!d->texture.empty()) {
            const TextureAsset* tex = texture(d->texture);
            if (!tex) { world_.destroy(e); error = "texture '" + d->texture + "' not found"; return {}; }
            sp.texture = assets_->find(d->texture).bits;
            int r = d->region.empty() ? 0 : tex->atlas.find(d->region);
            sp.atlas_region = r < 0 ? 0 : uint32_t(r);
        }
        sp.size = d->sprite_size; sp.layer = d->layer; sp.depth = d->depth; sp.tint = d->tint;
        world_.sprites.add(e, sp);
    }
    if (d->has_body) {
        BodyDef b = d->body;
        b.position = pos;
        b.user = e.bits;
        world_.bodies.add(e, BodyC{physics_.create_body(b).bits});
    }
    if (!d->animation.empty()) world_.animators.add(e, AnimatorC{anim_.create_player(d->animation)});
    return e;
}

bool Engine::destroy_entity(EntityHandle e) { return world_.destroy(e); }

// ------------------------------------------------------------------------------ input injection

uint64_t Engine::next_tick_input_ns() const {
    return sim_origin_ns_ + uint64_t(sim_.tick) * 1000000000ull / sim_.ticks_per_second + 1;
}

void Engine::inject(uint16_t code, bool down) {
    InputEvent e;
    e.timestamp_ns = next_tick_input_ns();
    e.type = down ? InputEventType::ButtonDown : InputEventType::ButtonUp;
    e.code = code;
    input_.push(e);
}

// ------------------------------------------------------------------------------ save states

namespace {
void write_color(BinaryWriter& w, Color c) { w.u8(c.r); w.u8(c.g); w.u8(c.b); w.u8(c.a); }
Color read_color(BinaryReader& r) { Color c; c.r = r.u8(); c.g = r.u8(); c.b = r.u8(); c.a = r.u8(); return c; }
void write_transform(BinaryWriter& w, const Transform& t) { w.vec2(t.position); w.f32(t.rotation); w.vec2(t.scale); }
Transform read_transform(BinaryReader& r) { Transform t; t.position = r.vec2(); t.rotation = r.f32(); t.scale = r.vec2(); return t; }
}  // namespace

std::vector<uint8_t> Engine::save_state(bool include_audio) {
    BinaryWriter w;
    lua_State* L = lua_->L();
    size_t sec;

    sec = w.begin_section("HEAD");
    w.str("UNIFY-SAVE"); w.u32(kSaveVersion); w.str(kEngineVersion); w.u32(sim_.ticks_per_second); w.u64(config_.seed);
    w.end_section(sec);

    sec = w.begin_section("TIME");
    w.u64(sim_.tick);
    w.u32(uint32_t(timers_.size()));
    for (auto& t : timers_) { w.u64(t.fire_tick); w.u64(t.interval_ticks); w.str(t.name); w.u16(t.scene); }
    w.end_section(sec);

    sec = w.begin_section("RNG ");
    for (int i = 0; i < 4; i++) w.u32(rng_.state()[i]);
    w.end_section(sec);

    sec = w.begin_section("SCEN");
    w.u16(next_scene_id_);
    w.u32(uint32_t(scenes_.size()));
    for (auto& s : scenes_) {
        w.u16(s.id); w.str(s.name);
        w.u32(uint32_t(s.assets.size()));
        for (AssetHandle a : s.assets) { const AssetRecord* r = assets_->get(a); w.u8(uint8_t(r->type)); w.str(r->name); }
        w.u32(uint32_t(s.buffers.size())); for (auto b : s.buffers) w.u32(b.bits);
        w.u32(uint32_t(s.kernels.size())); for (auto k : s.kernels) w.u32(k.bits);
    }
    w.end_section(sec);

    sec = w.begin_section("ECS ");
    {
        auto es = world_.entity_pool().save();
        w.u32(uint32_t(es.gens.size()));
        for (size_t i = 0; i < es.gens.size(); i++) {
            w.u8(es.gens[i]); w.u8(es.live[i]);
            if (es.live[i]) { const EntityInfo& in = world_.infos()[i]; w.str(in.name); w.str(in.prefab); w.u16(in.scene); w.u32(in.tags); }
        }
        w.u32(uint32_t(es.free.size()));
        for (auto f : es.free) w.u32(f);
        // Component pools in dense order (their order is part of the deterministic state).
        w.u32(uint32_t(world_.transforms.size()));
        for (size_t i = 0; i < world_.transforms.size(); i++) {
            w.u32(world_.transforms.owner(i).bits);
            write_transform(w, world_.transforms.at(i).current);
            write_transform(w, world_.transforms.at(i).previous);
        }
        w.u32(uint32_t(world_.sprites.size()));
        for (size_t i = 0; i < world_.sprites.size(); i++) {
            const SpriteC& s = world_.sprites.at(i);
            w.u32(world_.sprites.owner(i).bits);
            const AssetRecord* tex = assets_->get(AssetHandle{s.texture});
            w.str(tex ? tex->name : "");  // assets by name: handles are not stable across runs
            w.u32(s.atlas_region); w.vec2(s.size); w.vec2(s.pivot); write_color(w, s.tint);
            w.u16(uint16_t(s.layer)); w.f32(s.depth); w.boolean(s.flip_x); w.boolean(s.visible);
        }
        w.u32(uint32_t(world_.bodies.size()));
        for (size_t i = 0; i < world_.bodies.size(); i++) { w.u32(world_.bodies.owner(i).bits); w.u32(world_.bodies.at(i).body); }
        w.u32(uint32_t(world_.animators.size()));
        for (size_t i = 0; i < world_.animators.size(); i++) { w.u32(world_.animators.owner(i).bits); w.u32(world_.animators.at(i).player); }
    }
    w.end_section(sec);

    sec = w.begin_section("PHYS"); physics_.serialize(w); w.end_section(sec);
    sec = w.begin_section("ANIM"); anim_.serialize(w); w.end_section(sec);
    sec = w.begin_section("INPT"); input_.serialize(w); w.end_section(sec);
    sec = w.begin_section("CAMR"); w.vec2(camera_.position); w.f32(camera_.zoom); w.f32(camera_.rotation); w.end_section(sec);
    sec = w.begin_section("COMP"); compute_.serialize(w); w.end_section(sec);

    // Lua: the global `game` table plus each scene's `state` field. Functions are code, not state:
    // they come back from the scene scripts themselves.
    sec = w.begin_section("LUA ");
    {
        std::string err;
        lua_getglobal(L, "game");
        if (!lua_->serialize_value(-1, w, err)) UNIFY_LOG_ERROR("SAVE", "game table: %s", err.c_str());
        lua_pop(L, 1);
        w.u32(uint32_t(scenes_.size()));
        for (auto& s : scenes_) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, s.lua_ref);
            lua_getfield(L, -1, "state");
            if (!lua_->serialize_value(-1, w, err)) UNIFY_LOG_ERROR("SAVE", "scene '%s' state: %s", s.name.c_str(), err.c_str());
            lua_pop(L, 2);
        }
    }
    w.end_section(sec);

    if (include_audio) {
        sec = w.begin_section("AUDI");
        auto voices = mixer_.snapshot();
        w.u32(uint32_t(voices.size()));
        for (auto& v : voices) {
            uint16_t owner = 0;
            for (auto& s : scenes_) if (std::find(s.voices.begin(), s.voices.end(), v.handle) != s.voices.end()) owner = s.id;
            w.str(v.asset); w.boolean(v.is_stream); w.u8(uint8_t(v.bus)); w.f32(v.volume); w.f32(v.pan); w.f32(v.pitch);
            w.boolean(v.loop); w.boolean(v.paused); w.f64(v.position); w.u16(owner);
        }
        w.f32(mixer_.bus_volume(Bus::Master)); w.f32(mixer_.bus_volume(Bus::Sfx)); w.f32(mixer_.bus_volume(Bus::Music));
        w.end_section(sec);
    }
    return w.take();
}

bool Engine::load_state(const std::vector<uint8_t>& bytes, std::string& error) {
    std::map<std::string, BinaryReader> sections;
    try {
        BinaryReader all(bytes);
        char tag[5];
        BinaryReader body(nullptr, 0);
        while (all.next_section(tag, body)) sections.emplace(tag, body);
        for (const char* need : {"HEAD", "TIME", "RNG ", "SCEN", "ECS ", "PHYS", "ANIM", "INPT", "CAMR", "COMP", "LUA "})
            if (!sections.count(need)) { error = std::string("save state is missing section ") + need; return false; }
        BinaryReader& head = sections.at("HEAD");
        if (head.str() != "UNIFY-SAVE") { error = "not a UNIFY save state"; return false; }
        uint32_t version = head.u32();
        if (version > kSaveVersion) { error = "save state version " + std::to_string(version) + " is newer than this engine"; return false; }
        head.str();
        if (head.u32() != sim_.ticks_per_second) { error = "save state was recorded at a different tick rate"; return false; }
    } catch (const SerializationError& e) {
        error = std::string("corrupt save state: ") + e.what();
        return false;
    }

    lua_State* L = lua_->L();
    try {
        // Keep every currently loaded asset alive across the swap so unchanged assets keep
        // their handles and are not reloaded from disk.
        std::vector<AssetHandle> keep;
        for (auto& s : scenes_) for (auto a : s.assets) { assets_->acquire(a); keep.push_back(a); }

        // Tear down the live world completely; the saved one replaces it.
        mixer_.stop_all();
        for (auto& s : scenes_) {
            s.voices.clear(); s.buffers.clear(); s.kernels.clear();  // compute state is replaced wholesale below
        }
        world_.clear();
        physics_.clear();
        while (!scenes_.empty()) { unload_scene(scenes_.back()); scenes_.pop_back(); }

        BinaryReader& time = sections.at("TIME");
        sim_.tick = time.u64();
        std::vector<Timer> saved_timers;
        uint32_t nt = time.u32();
        for (uint32_t i = 0; i < nt; i++) { Timer t; t.fire_tick = time.u64(); t.interval_ticks = time.u64(); t.name = time.str(); t.scene = time.u16(); saved_timers.push_back(t); }

        BinaryReader& rng = sections.at("RNG ");
        uint32_t st[4];
        for (auto& x : st) x = rng.u32();
        rng_.set_state(st);

        std::string cerr;
        if (!compute_.deserialize(sections.at("COMP"), cerr)) { error = cerr; return false; }

        BinaryReader& sc = sections.at("SCEN");
        next_scene_id_ = sc.u16();
        uint32_t ns = sc.u32();
        for (uint32_t i = 0; i < ns; i++) {
            Scene s;
            s.id = sc.u16();
            s.name = sc.str();
            s.voices.reserve(64);
            scenes_.push_back(s);
            Scene& live = scenes_.back();
            if (!load_scene_script(live, error)) return false;
            // Restore the scene's asset list before init(), so init() finds its assets already
            // owned instead of appending duplicates (which would change the saved state).
            uint32_t na = sc.u32();
            for (uint32_t k = 0; k < na; k++) {
                AssetType type = AssetType(sc.u8());
                std::string name = sc.str();
                AssetHandle h = assets_->load(type, name);
                if (!h) { error = "asset '" + name + "' from the save state could not be loaded"; return false; }
                live.assets.push_back(h);
            }
            if (!call_scene("init")) { error = "scene init: " + lua_->last_error(); return false; }
            uint32_t nb = sc.u32(); for (uint32_t k = 0; k < nb; k++) live.buffers.push_back(BufferHandle{sc.u32()});
            uint32_t nk = sc.u32(); for (uint32_t k = 0; k < nk; k++) live.kernels.push_back(KernelHandle{sc.u32()});
        }
        for (auto a : keep) assets_->release(a);
        timers_ = saved_timers;

        BinaryReader& ecs = sections.at("ECS ");
        {
            HandlePool<EntityHandle>::State es;
            uint32_t n = ecs.u32();
            es.gens.resize(n); es.live.resize(n);
            world_.infos().assign(n, EntityInfo{});
            for (uint32_t i = 0; i < n; i++) {
                es.gens[i] = ecs.u8(); es.live[i] = ecs.u8();
                if (!es.live[i]) continue;
                es.alive++;
                EntityInfo& in = world_.infos()[i];
                in.name = ecs.str(); in.prefab = ecs.str(); in.scene = ecs.u16(); in.tags = ecs.u32();
            }
            es.free.resize(ecs.u32());
            for (auto& f : es.free) f = ecs.u32();
            world_.entity_pool().load(es);
            uint32_t nt2 = ecs.u32();
            for (uint32_t i = 0; i < nt2; i++) {
                EntityHandle e{ecs.u32()};
                TransformC t; t.current = read_transform(ecs); t.previous = read_transform(ecs);
                world_.transforms.add(e, t);
            }
            uint32_t nsp = ecs.u32();
            for (uint32_t i = 0; i < nsp; i++) {
                EntityHandle e{ecs.u32()};
                SpriteC s;
                std::string tex = ecs.str();
                s.texture = tex.empty() ? 0 : assets_->find(tex).bits;
                s.atlas_region = ecs.u32(); s.size = ecs.vec2(); s.pivot = ecs.vec2(); s.tint = read_color(ecs);
                s.layer = int16_t(ecs.u16()); s.depth = ecs.f32(); s.flip_x = ecs.boolean(); s.visible = ecs.boolean();
                world_.sprites.add(e, s);
            }
            uint32_t nb = ecs.u32();
            for (uint32_t i = 0; i < nb; i++) { EntityHandle e{ecs.u32()}; world_.bodies.add(e, BodyC{ecs.u32()}); }
            uint32_t na = ecs.u32();
            for (uint32_t i = 0; i < na; i++) { EntityHandle e{ecs.u32()}; world_.animators.add(e, AnimatorC{ecs.u32()}); }
        }

        physics_.deserialize(sections.at("PHYS"));
        anim_.deserialize(sections.at("ANIM"));
        input_.deserialize(sections.at("INPT"));
        BinaryReader& cam = sections.at("CAMR");
        camera_.position = cam.vec2(); camera_.zoom = cam.f32(); camera_.rotation = cam.f32();

        BinaryReader& lr = sections.at("LUA ");
        std::string lerr;
        if (!lua_->deserialize_value(lr, lerr)) { error = "game table: " + lerr; return false; }
        lua_setglobal(L, "game");
        uint32_t nls = lr.u32();
        for (uint32_t i = 0; i < nls && i < scenes_.size(); i++) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, scenes_[i].lua_ref);
            if (!lua_->deserialize_value(lr, lerr)) { lua_pop(L, 1); error = "scene state: " + lerr; return false; }
            lua_setfield(L, -2, "state");
            lua_pop(L, 1);
        }

        if (sections.count("AUDI")) {
            BinaryReader& au = sections.at("AUDI");
            uint32_t nv = au.u32();
            for (uint32_t i = 0; i < nv; i++) {
                std::string asset = au.str();
                bool is_stream = au.boolean();
                PlayParams p;
                p.bus = Bus(au.u8()); p.volume = au.f32(); p.pan = au.f32(); p.pitch = au.f32(); p.loop = au.boolean();
                bool paused = au.boolean();
                double pos = au.f64();
                uint16_t owner = au.u16();
                AudioHandle h;
                if (is_stream) {
                    auto src = assets_->open_stream(assets_->find(asset));
                    if (src) { src->seek(uint32_t(std::max(0.0, pos))); h = mixer_.stream(src, p); }
                } else if (const AssetRecord* r = assets_->get(assets_->find(asset))) {
                    p.start = pos;
                    h = mixer_.play(r->clip, p);
                }
                if (h && paused) mixer_.pause(h);
                for (auto& s : scenes_) if (s.id == owner && h) s.voices.push_back(h);
            }
            mixer_.set_bus_volume(Bus::Master, au.f32()); mixer_.set_bus_volume(Bus::Sfx, au.f32()); mixer_.set_bus_volume(Bus::Music, au.f32());
        }
    } catch (const SerializationError& e) {
        error = std::string("corrupt save state: ") + e.what();
        return false;
    }
    // Real time continues from the restored tick.
    // Same exact formula as tick boundaries (SimClock::tick_end_ns); a truncated per-tick
    // duration here would drift input windows by up to a tick after restoring late in a session.
    sim_origin_ns_ = backend_.now_ns() - sim_.tick * 1000000000ull / sim_.ticks_per_second;
    lua_->gc_collect();
    return true;
}

uint64_t Engine::state_hash() {
    std::vector<uint8_t> s = save_state(false);
    return fnv1a(s.data(), s.size());
}

bool Engine::save_to_file(const std::string& slot, std::string& error) {
    std::string path = assets_->path_for(AssetType::SaveData, "saves/" + slot);
    if (!write_file(path, save_state())) { error = "could not write " + path; return false; }
    return true;
}

bool Engine::load_from_file(const std::string& slot, std::string& error) {
    std::vector<uint8_t> data;
    std::string path = assets_->path_for(AssetType::SaveData, "saves/" + slot);
    if (!read_file(path, data)) { error = "no save at " + path; return false; }
    return load_state(data, error);
}

void Engine::process_pending_state_ops() {
    // Requests made during the tick run here, at the tick boundary, in request order:
    // state is never captured or replaced halfway through a simulation step.
    std::vector<std::pair<bool, std::string>> ops;
    ops.swap(pending_ops_);
    for (auto& [is_save, slot] : ops) {
        std::string err;
        if (is_save) {
            bool ok = true;
            if (slot.empty()) quick_slot_ = save_state();
            else if (!(ok = save_to_file(slot, err))) UNIFY_LOG_ERROR("SAVE", "%s", err.c_str());
            lua_pushboolean(lua_->L(), ok);
            lua_pushstring(lua_->L(), slot.c_str());
            if (!call_scene("on_saved", 2)) report_script_error("scene on_saved");
        } else {
            bool ok = slot.empty() ? (!quick_slot_.empty() && load_state(quick_slot_, err)) : load_from_file(slot, err);
            if (!ok) UNIFY_LOG_ERROR("SAVE", "load failed: %s", err.empty() ? "no quick save" : err.c_str());
            lua_pushboolean(lua_->L(), ok);
            lua_pushstring(lua_->L(), slot.c_str());
            if (!call_scene("on_loaded", 2)) report_script_error("scene on_loaded");
        }
    }
}

}  // namespace unify
