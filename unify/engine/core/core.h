#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <string>

namespace unify {

// ---------------------------------------------------------------------------------------
// Deterministic RNG. The ONLY random source simulation code may use (engine, physics, Lua).
// xoshiro128** — 128-bit state, fully serializable, identical output on every platform.
// ---------------------------------------------------------------------------------------
class Rng {
public:
    explicit Rng(uint64_t seed = 0x5eed) { reseed(seed); }
    void reseed(uint64_t seed) {
        // SplitMix64 expands the seed so nearby seeds give unrelated streams.
        for (int i = 0; i < 4; i += 2) {
            seed += 0x9E3779B97F4A7C15ull;
            uint64_t z = seed;
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
            z ^= z >> 31;
            s_[i] = uint32_t(z);
            s_[i + 1] = uint32_t(z >> 32);
        }
        if ((s_[0] | s_[1] | s_[2] | s_[3]) == 0) s_[0] = 1;
    }
    uint32_t next_u32() {
        uint32_t result = rotl(s_[1] * 5, 7) * 9;
        uint32_t t = s_[1] << 9;
        s_[2] ^= s_[0]; s_[3] ^= s_[1]; s_[1] ^= s_[2]; s_[0] ^= s_[3];
        s_[2] ^= t; s_[3] = rotl(s_[3], 11);
        return result;
    }
    /// [0, 1) with 24 bits of precision: exactly representable in float, no rounding drift.
    float next_float() { return float(next_u32() >> 8) * (1.0f / 16777216.0f); }
    float range(float lo, float hi) { return lo + (hi - lo) * next_float(); }
    int32_t range_int(int32_t lo, int32_t hi_inclusive) {
        uint32_t span = uint32_t(hi_inclusive - lo) + 1;
        return span == 0 ? int32_t(next_u32()) : lo + int32_t(next_u32() % span);
    }
    const uint32_t* state() const { return s_; }
    void set_state(const uint32_t st[4]) { for (int i = 0; i < 4; i++) s_[i] = st[i]; }

private:
    static uint32_t rotl(uint32_t x, int k) { return (x << k) | (x >> (32 - k)); }
    uint32_t s_[4];
};

// ---------------------------------------------------------------------------------------
// Clocks. Five independent notions of time; only SimClock is visible to gameplay.
// ---------------------------------------------------------------------------------------

/// Wall clock: platform monotonic time in nanoseconds. Nondeterministic; never read by simulation.
uint64_t wall_clock_ns();

/// Simulation time: an integer tick counter. Seconds are derived, never accumulated,
/// so there is no floating-point drift across long sessions or save/restore.
struct SimClock {
    uint64_t tick = 0;
    uint32_t ticks_per_second = 60;
    double dt() const { return 1.0 / ticks_per_second; }
    float dtf() const { return 1.0f / float(ticks_per_second); }
    double seconds() const { return double(tick) / ticks_per_second; }
    /// Input timestamps are nanoseconds since engine start on the input clock; tick N covers
    /// the half-open interval [N*dt, (N+1)*dt).
    uint64_t tick_end_ns(uint64_t t) const { return (t + 1) * 1000000000ull / ticks_per_second; }
};

/// Render time: wall time of the current frame plus the interpolation factor between the
/// previous and current simulation states.
struct RenderClock {
    uint64_t frame = 0;
    double seconds = 0;
    float alpha = 0;       // 0..1 blend between previous and current sim state
    double frame_ms = 0;
};

// ---------------------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------------------
enum class LogLevel { Debug, Info, Warn, Error };
void log_message(LogLevel level, const char* category, const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 3, 4)))
#endif
    ;
void set_log_level(LogLevel min_level);

#define UNIFY_LOG_INFO(cat, ...) ::unify::log_message(::unify::LogLevel::Info, cat, __VA_ARGS__)
#define UNIFY_LOG_WARN(cat, ...) ::unify::log_message(::unify::LogLevel::Warn, cat, __VA_ARGS__)
#define UNIFY_LOG_ERROR(cat, ...) ::unify::log_message(::unify::LogLevel::Error, cat, __VA_ARGS__)
#define UNIFY_LOG_DEBUG(cat, ...) ::unify::log_message(::unify::LogLevel::Debug, cat, __VA_ARGS__)

/// FNV-1a 64 over bytes; used for state hashes (determinism checks, rollback verification).
inline uint64_t fnv1a(const void* data, size_t n, uint64_t h = 1469598103934665603ull) {
    auto p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

}  // namespace unify
