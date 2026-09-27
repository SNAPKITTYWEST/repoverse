#include "animation.h"
#include "../serialization/stream.h"
#include <algorithm>
#include <cmath>

namespace unify {

float Track::sample(float t) const {
    if (keys.empty()) return 0;
    if (t <= keys.front().time) return keys.front().value;
    if (t >= keys.back().time) return keys.back().value;
    auto it = std::upper_bound(keys.begin(), keys.end(), t, [](float x, const Keyframe& k) { return x < k.time; });
    const Keyframe& b = *it;
    const Keyframe& a = *(it - 1);
    float span = b.time - a.time;
    return span > 0 ? a.value + (b.value - a.value) * ((t - a.time) / span) : b.value;
}

float AnimationClip::length() const {
    float frames_len = 0;
    for (auto& f : frames) frames_len += f.duration;
    float track_len = 0;
    for (auto& t : tracks) if (!t.keys.empty()) track_len = std::max(track_len, t.keys.back().time);
    return std::max(frames_len, track_len);
}

void AnimationSystem::add_clip(AnimationClip c) { clips_[c.name] = std::move(c); }
const AnimationClip* AnimationSystem::clip(const std::string& name) const {
    auto it = clips_.find(name);
    return it == clips_.end() ? nullptr : &it->second;
}

uint32_t AnimationSystem::create_player(const std::string& clip) {
    uint32_t id;
    if (!free_.empty()) { id = free_.back(); free_.pop_back(); }
    else { id = uint32_t(players_.size()); players_.emplace_back(); live_.push_back(0); }
    players_[id] = AnimationPlayer{};
    players_[id].clip = clip;
    live_[id] = 1;
    return id + 1;  // 0 means "no player"
}
bool AnimationSystem::destroy_player(uint32_t id) {
    if (id == 0 || id > players_.size() || !live_[id - 1]) return false;
    live_[id - 1] = 0;
    free_.push_back(id - 1);
    return true;
}
AnimationPlayer* AnimationSystem::player(uint32_t id) {
    return (id == 0 || id > players_.size() || !live_[id - 1]) ? nullptr : &players_[id - 1];
}
size_t AnimationSystem::live_players() const { return size_t(std::count(live_.begin(), live_.end(), 1)); }

void AnimationSystem::play(uint32_t id, const std::string& name, float blend_seconds, bool restart) {
    AnimationPlayer* p = player(id);
    if (!p) return;
    if (p->clip == name && !restart) return;
    if (blend_seconds > 0) {
        p->from_clip = p->clip;
        p->from_time = p->time;
        p->blend = 0;
        p->blend_duration = blend_seconds;
    } else {
        p->from_clip.clear();
        p->blend = 1;
    }
    p->clip = name;
    p->time = 0;
    p->finished = false;
}

// Maps an unbounded play time into clip time according to the play mode.
float AnimationSystem::wrap(const AnimationClip& c, float t, bool* finished) {
    float len = c.length();
    if (len <= 0) return 0;
    switch (c.mode) {
        case PlayMode::Once:
            if (t >= len) { if (finished) *finished = true; return len; }
            return t;
        case PlayMode::Loop: return std::fmod(t, len);
        case PlayMode::PingPong: {
            float m = std::fmod(t, 2 * len);
            return m <= len ? m : 2 * len - m;
        }
    }
    return t;
}

void AnimationSystem::advance(float dt) {
    for (size_t i = 0; i < players_.size(); i++) {
        if (!live_[i]) continue;
        AnimationPlayer& p = players_[i];
        const AnimationClip* c = clip(p.clip);
        if (!c) continue;
        if (!p.finished) {
            p.time += dt * p.speed;
            if (c->mode == PlayMode::Once && p.time >= c->length()) { p.time = c->length(); p.finished = true; }
            // Keep loop time bounded so float precision does not degrade over long sessions.
            if (c->mode == PlayMode::Loop && c->length() > 0) p.time = std::fmod(p.time, c->length());
            if (c->mode == PlayMode::PingPong && c->length() > 0) p.time = std::fmod(p.time, 2 * c->length());
        }
        if (p.blend < 1) {
            p.from_time += dt * p.speed;
            p.blend = p.blend_duration > 0 ? std::min(1.0f, p.blend + dt / p.blend_duration) : 1.0f;
            if (p.blend >= 1) p.from_clip.clear();
        }
    }
}

uint32_t AnimationSystem::sprite_region(uint32_t id) const {
    const AnimationPlayer* p = const_cast<AnimationSystem*>(this)->player(id);
    if (!p) return 0;
    const AnimationClip* c = clip(p->clip);
    if (!c || c->frames.empty()) return 0;
    float t = wrap(*c, p->time, nullptr);
    float acc = 0;
    for (auto& f : c->frames) {
        acc += f.duration;
        if (t < acc) return f.region;
    }
    return c->frames.back().region;
}

float AnimationSystem::value(uint32_t id, const std::string& property, float fallback) const {
    const AnimationPlayer* p = const_cast<AnimationSystem*>(this)->player(id);
    if (!p) return fallback;
    auto sample = [&](const std::string& name, float time, bool& found) {
        const AnimationClip* c = clip(name);
        if (!c) return fallback;
        for (auto& tr : c->tracks)
            if (tr.property == property) { found = true; return tr.sample(wrap(*c, time, nullptr)); }
        return fallback;
    };
    bool found_to = false, found_from = false;
    float to = sample(p->clip, p->time, found_to);
    if (p->blend >= 1 || p->from_clip.empty()) return to;
    float from = sample(p->from_clip, p->from_time, found_from);
    if (!found_to && !found_from) return fallback;
    return from + (to - from) * p->blend;
}

void AnimationSystem::serialize(BinaryWriter& w) const {
    w.u32(uint32_t(players_.size()));
    for (size_t i = 0; i < players_.size(); i++) {
        w.u8(live_[i]);
        if (!live_[i]) continue;
        const AnimationPlayer& p = players_[i];
        w.str(p.clip); w.f32(p.time); w.f32(p.speed); w.boolean(p.finished);
        w.str(p.from_clip); w.f32(p.from_time); w.f32(p.blend); w.f32(p.blend_duration);
    }
    w.u32(uint32_t(free_.size()));
    for (uint32_t f : free_) w.u32(f);
}

void AnimationSystem::deserialize(BinaryReader& r) {
    uint32_t n = r.u32();
    players_.assign(n, AnimationPlayer{});
    live_.assign(n, 0);
    for (uint32_t i = 0; i < n; i++) {
        live_[i] = r.u8();
        if (!live_[i]) continue;
        AnimationPlayer& p = players_[i];
        p.clip = r.str(); p.time = r.f32(); p.speed = r.f32(); p.finished = r.boolean();
        p.from_clip = r.str(); p.from_time = r.f32(); p.blend = r.f32(); p.blend_duration = r.f32();
    }
    free_.resize(r.u32());
    for (auto& f : free_) f = r.u32();
}

}  // namespace unify
