#pragma once
#include <cstdint>
#include <vector>
#include <cassert>
#include <functional>

namespace unify {

/// Generational handle: 24-bit slot index + 8-bit generation, packed in 32 bits.
/// Handles are what cross subsystem and script boundaries; raw pointers never do.
/// A handle whose generation no longer matches its slot is stale and fails validation.
template <typename Tag>
struct Handle {
    uint32_t bits = 0;  // 0 == null (generation 0 is never issued)
    static constexpr uint32_t kIndexBits = 24;
    static constexpr uint32_t kIndexMask = (1u << kIndexBits) - 1;

    static Handle make(uint32_t index, uint8_t gen) { return Handle{(uint32_t(gen) << kIndexBits) | (index & kIndexMask)}; }
    uint32_t index() const { return bits & kIndexMask; }
    uint8_t generation() const { return uint8_t(bits >> kIndexBits); }
    bool is_null() const { return bits == 0; }
    explicit operator bool() const { return bits != 0; }
    bool operator==(const Handle& o) const { return bits == o.bits; }
    bool operator!=(const Handle& o) const { return bits != o.bits; }
    bool operator<(const Handle& o) const { return bits < o.bits; }
};

struct EntityTag {};
struct TextureTag {};
struct AudioTag {};
struct PhysicsBodyTag {};
struct BufferTag {};
struct KernelTag {};
struct AssetTag {};
struct ClipTag {};

using EntityHandle = Handle<EntityTag>;
using TextureHandle = Handle<TextureTag>;
using AudioHandle = Handle<AudioTag>;        // a playing voice
using PhysicsBodyHandle = Handle<PhysicsBodyTag>;
using BufferHandle = Handle<BufferTag>;
using KernelHandle = Handle<KernelTag>;
using AssetHandle = Handle<AssetTag>;

/// Issues and validates generational handles over a dense slot range with a free list.
/// Free order is LIFO and fully deterministic, so the same sequence of alloc/free calls
/// always yields the same handles (required for deterministic replay and save states).
template <typename H>
class HandlePool {
public:
    H allocate() {
        uint32_t index;
        if (!free_.empty()) {
            index = free_.back();
            free_.pop_back();
        } else {
            index = uint32_t(gens_.size());
            assert(index <= H::kIndexMask);
            gens_.push_back(1);
            live_.push_back(0);
        }
        live_[index] = 1;
        alive_++;
        return H::make(index, gens_[index]);
    }

    bool valid(H h) const {
        uint32_t i = h.index();
        return !h.is_null() && i < gens_.size() && live_[i] && gens_[i] == h.generation();
    }

    bool release(H h) {
        if (!valid(h)) return false;
        uint8_t& g = gens_[h.index()];
        g = uint8_t(g + 1);
        if (g == 0) g = 1;  // skip 0 so a recycled slot never re-issues null
        live_[h.index()] = 0;
        free_.push_back(h.index());
        alive_--;
        return true;
    }

    /// Handle currently occupying a slot (for iteration); null if the slot is free.
    H at(uint32_t index) const { return index < gens_.size() && live_[index] ? H::make(index, gens_[index]) : H{}; }

    uint32_t alive() const { return alive_; }
    uint32_t capacity() const { return uint32_t(gens_.size()); }
    void reserve(size_t n) { gens_.reserve(n); free_.reserve(n); live_.reserve(n); }

    /// Raw state for save/restore: generations, liveness and free-list order.
    struct State { std::vector<uint8_t> gens; std::vector<uint8_t> live; std::vector<uint32_t> free; uint32_t alive = 0; };
    State save() const { return {gens_, live_, free_, alive_}; }
    void load(const State& s) { gens_ = s.gens; live_ = s.live; free_ = s.free; alive_ = s.alive; }
    void clear() { gens_.clear(); live_.clear(); free_.clear(); alive_ = 0; }

private:
    std::vector<uint8_t> gens_;
    std::vector<uint8_t> live_;
    std::vector<uint32_t> free_;
    uint32_t alive_ = 0;
};

}  // namespace unify

namespace std {
template <typename T>
struct hash<unify::Handle<T>> {
    size_t operator()(const unify::Handle<T>& h) const noexcept { return std::hash<uint32_t>()(h.bits); }
};
}  // namespace std
