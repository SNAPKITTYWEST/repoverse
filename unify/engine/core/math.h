#pragma once
#include <cmath>
#include <cstdint>
#include <algorithm>

namespace unify {

struct Vec2 {
    float x = 0, y = 0;
    constexpr Vec2() = default;
    constexpr Vec2(float x_, float y_) : x(x_), y(y_) {}
    Vec2 operator+(Vec2 o) const { return {x + o.x, y + o.y}; }
    Vec2 operator-(Vec2 o) const { return {x - o.x, y - o.y}; }
    Vec2 operator-() const { return {-x, -y}; }
    Vec2 operator*(float s) const { return {x * s, y * s}; }
    Vec2 operator/(float s) const { return {x / s, y / s}; }
    Vec2& operator+=(Vec2 o) { x += o.x; y += o.y; return *this; }
    Vec2& operator-=(Vec2 o) { x -= o.x; y -= o.y; return *this; }
    Vec2& operator*=(float s) { x *= s; y *= s; return *this; }
    bool operator==(Vec2 o) const { return x == o.x && y == o.y; }
    bool operator!=(Vec2 o) const { return !(*this == o); }
};
inline Vec2 operator*(float s, Vec2 v) { return {v.x * s, v.y * s}; }
inline float dot(Vec2 a, Vec2 b) { return a.x * b.x + a.y * b.y; }
inline float cross(Vec2 a, Vec2 b) { return a.x * b.y - a.y * b.x; }
inline Vec2 cross(Vec2 a, float s) { return {s * a.y, -s * a.x}; }
inline Vec2 cross(float s, Vec2 a) { return {-s * a.y, s * a.x}; }
inline float length_sq(Vec2 v) { return dot(v, v); }
inline float length(Vec2 v) { return std::sqrt(dot(v, v)); }
inline Vec2 perp(Vec2 v) { return {-v.y, v.x}; }
inline Vec2 normalize(Vec2 v) {
    float l = length(v);
    return l > 1e-12f ? v / l : Vec2{0, 0};
}
inline Vec2 vmin(Vec2 a, Vec2 b) { return {std::min(a.x, b.x), std::min(a.y, b.y)}; }
inline Vec2 vmax(Vec2 a, Vec2 b) { return {std::max(a.x, b.x), std::max(a.y, b.y)}; }
inline bool finite(Vec2 v) { return std::isfinite(v.x) && std::isfinite(v.y); }
inline float lerp(float a, float b, float t) { return a + (b - a) * t; }
inline Vec2 lerp(Vec2 a, Vec2 b, float t) { return a + (b - a) * t; }
inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

/// 2D rotation stored as cos/sin so composing and inverting never calls trig.
struct Rot {
    float c = 1, s = 0;
    Rot() = default;
    explicit Rot(float angle) : c(std::cos(angle)), s(std::sin(angle)) {}
    float angle() const { return std::atan2(s, c); }
    Vec2 apply(Vec2 v) const { return {c * v.x - s * v.y, s * v.x + c * v.y}; }
    Vec2 apply_inv(Vec2 v) const { return {c * v.x + s * v.y, -s * v.x + c * v.y}; }
};

struct Transform {
    Vec2 position;
    float rotation = 0;   // radians
    Vec2 scale{1, 1};
    Vec2 apply(Vec2 local) const {
        Rot r(rotation);
        return position + r.apply({local.x * scale.x, local.y * scale.y});
    }
};

struct AABB {
    Vec2 min, max;
    bool overlaps(const AABB& o) const {
        return !(max.x < o.min.x || o.max.x < min.x || max.y < o.min.y || o.max.y < min.y);
    }
    AABB merged(const AABB& o) const { return {vmin(min, o.min), vmax(max, o.max)}; }
    AABB expanded(float r) const { return {{min.x - r, min.y - r}, {max.x + r, max.y + r}}; }
    Vec2 center() const { return (min + max) * 0.5f; }
    Vec2 extent() const { return (max - min) * 0.5f; }
};

struct Color {
    uint8_t r = 255, g = 255, b = 255, a = 255;
    static constexpr Color rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) { return Color{r, g, b, a}; }
    bool operator==(const Color& o) const { return r == o.r && g == o.g && b == o.b && a == o.a; }
};

}  // namespace unify
