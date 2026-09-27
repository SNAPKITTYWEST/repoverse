#pragma once
#include <vector>
#include <string>
#include <cstdint>
#include <functional>
#include "../core/handle.h"
#include "../core/math.h"

namespace unify {

/// Sparse-set component storage. Dense arrays give cache-friendly, deterministic
/// iteration (order depends only on the sequence of add/remove calls).
template <typename T>
class ComponentPool {
public:
    static constexpr uint32_t kNone = 0xFFFFFFFFu;

    T& add(EntityHandle e, T value = T{}) {
        uint32_t i = e.index();
        if (i >= sparse_.size()) sparse_.resize(i + 1, kNone);
        if (sparse_[i] != kNone) { dense_[sparse_[i]] = std::move(value); owners_[sparse_[i]] = e; return dense_[sparse_[i]]; }
        sparse_[i] = uint32_t(dense_.size());
        dense_.push_back(std::move(value));
        owners_.push_back(e);
        return dense_.back();
    }
    bool remove(EntityHandle e) {
        uint32_t i = e.index();
        if (i >= sparse_.size() || sparse_[i] == kNone || owners_[sparse_[i]] != e) return false;
        uint32_t slot = sparse_[i];
        uint32_t last = uint32_t(dense_.size() - 1);
        if (slot != last) {
            dense_[slot] = std::move(dense_[last]);
            owners_[slot] = owners_[last];
            sparse_[owners_[slot].index()] = slot;
        }
        dense_.pop_back();
        owners_.pop_back();
        sparse_[i] = kNone;
        return true;
    }
    T* get(EntityHandle e) {
        uint32_t i = e.index();
        if (i >= sparse_.size() || sparse_[i] == kNone || owners_[sparse_[i]] != e) return nullptr;
        return &dense_[sparse_[i]];
    }
    const T* get(EntityHandle e) const { return const_cast<ComponentPool*>(this)->get(e); }
    bool has(EntityHandle e) const { return get(e) != nullptr; }
    size_t size() const { return dense_.size(); }
    T& at(size_t i) { return dense_[i]; }
    const T& at(size_t i) const { return dense_[i]; }
    EntityHandle owner(size_t i) const { return owners_[i]; }
    void clear() { dense_.clear(); owners_.clear(); sparse_.clear(); }
    void reserve(size_t n) { dense_.reserve(n); owners_.reserve(n); sparse_.reserve(n); }
    template <typename F> void each(F&& f) { for (size_t i = 0; i < dense_.size(); i++) f(owners_[i], dense_[i]); }
private:
    std::vector<T> dense_;
    std::vector<EntityHandle> owners_;
    std::vector<uint32_t> sparse_;
};

// ----- Engine components -------------------------------------------------------------

struct TransformC {
    Transform current;
    Transform previous;   // last tick's pose, for render interpolation
};

struct SpriteC {
    uint32_t texture = 0;      // TextureHandle bits
    uint32_t atlas_region = 0; // region index within the texture's atlas (0 = whole texture)
    Vec2 size{16, 16};         // world units
    Vec2 pivot{0.5f, 0.5f};
    Color tint{};
    int16_t layer = 0;
    float depth = 0;
    bool flip_x = false;
    bool visible = true;
};

struct BodyC { uint32_t body = 0; };       // PhysicsBodyHandle bits
struct AnimatorC { uint32_t player = 0; }; // index into AnimationSystem players

struct EntityInfo {
    std::string name;
    std::string prefab;
    uint16_t scene = 0;     // owning scene id; 0 = persistent across scene transitions
    uint32_t tags = 0;
};

/// The entity/component database. Owns entity lifetime; subsystems (physics, animation,
/// audio) own their own data and are linked to entities by handle through components.
class World {
public:
    World() { entities_.reserve(4096); }

    EntityHandle create(const std::string& name = {}, uint16_t scene = 0) {
        EntityHandle e = entities_.allocate();
        if (e.index() >= info_.size()) info_.resize(e.index() + 1);
        info_[e.index()] = EntityInfo{name, {}, scene, 0};
        transforms.add(e);
        return e;
    }
    /// Destroys an entity and its engine components. Subsystem resources are released by
    /// the on_destroy hooks (physics bodies, animation players, script refs) first.
    bool destroy(EntityHandle e) {
        if (!entities_.valid(e)) return false;
        for (auto& hook : on_destroy_) hook(e);
        transforms.remove(e);
        sprites.remove(e);
        bodies.remove(e);
        animators.remove(e);
        info_[e.index()] = {};
        entities_.release(e);
        return true;
    }
    bool valid(EntityHandle e) const { return entities_.valid(e); }
    EntityInfo* info(EntityHandle e) { return valid(e) ? &info_[e.index()] : nullptr; }
    uint32_t alive() const { return entities_.alive(); }
    uint32_t capacity() const { return entities_.capacity(); }
    EntityHandle at(uint32_t index) const { return entities_.at(index); }

    EntityHandle find(const std::string& name) const {
        for (uint32_t i = 0; i < entities_.capacity(); i++) {
            EntityHandle e = entities_.at(i);
            if (e && info_[i].name == name) return e;
        }
        return {};
    }
    template <typename F> void each_entity(F&& f) const {
        for (uint32_t i = 0; i < entities_.capacity(); i++) if (EntityHandle e = entities_.at(i)) f(e);
    }

    void add_destroy_hook(std::function<void(EntityHandle)> hook) { on_destroy_.push_back(std::move(hook)); }

    ComponentPool<TransformC> transforms;
    ComponentPool<SpriteC> sprites;
    ComponentPool<BodyC> bodies;
    ComponentPool<AnimatorC> animators;

    // Save-state access.
    HandlePool<EntityHandle>& entity_pool() { return entities_; }
    std::vector<EntityInfo>& infos() { return info_; }
    void clear() {
        entities_.clear(); info_.clear();
        transforms.clear(); sprites.clear(); bodies.clear(); animators.clear();
    }

private:
    HandlePool<EntityHandle> entities_;
    std::vector<EntityInfo> info_;
    std::vector<std::function<void(EntityHandle)>> on_destroy_;
};

}  // namespace unify
