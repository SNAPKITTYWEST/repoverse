#pragma once
#include <vector>
#include <map>
#include <algorithm>
#include <cstdint>
#include "../core/math.h"
#include "../core/handle.h"
#include "../memory/memory.h"

namespace unify {
class BinaryWriter;
class BinaryReader;

namespace phys {
constexpr float kLinearSlop = 0.005f;          // allowed penetration (m); keeps contacts warm
constexpr float kAngularSlop = 2.0f / 180.0f * 3.14159265f;
constexpr float kPolygonRadius = 2.0f * kLinearSlop;
constexpr float kMaxLinearCorrection = 0.2f;
constexpr float kBaumgarte = 0.2f;
constexpr float kVelocityThreshold = 1.0f;      // below this closing speed, no bounce
constexpr float kTimeToSleep = 0.5f;
constexpr float kLinearSleepTolerance = 0.01f;
constexpr float kAngularSleepTolerance = 2.0f / 180.0f * 3.14159265f;
constexpr int kMaxPolygonVertices = 8;
constexpr float kMaxTranslation = 50.0f;        // per step clamp against numeric explosions
}  // namespace phys

enum class ShapeType : uint8_t { Circle, AABB, Box, Polygon };
enum class BodyType : uint8_t { Static, Kinematic, Dynamic };

struct Shape {
    ShapeType type = ShapeType::Circle;
    float radius = 0.5f;                               // circle radius; polygon skin (kPolygonRadius)
    int count = 0;                                     // polygon vertex count
    Vec2 vertices[phys::kMaxPolygonVertices];          // body-local, CCW, centroid at origin
    Vec2 normals[phys::kMaxPolygonVertices];

    static Shape circle(float radius);
    static Shape aabb(float half_w, float half_h);      // axis-aligned: body rotation is locked
    static Shape box(float half_w, float half_h);       // oriented box: rotates freely
    /// Convex hull of the given points (≤ 8), recentred on its centroid. Returns the offset
    /// that was subtracted so callers can shift the body position to keep world placement.
    static Shape polygon(const Vec2* points, int n, Vec2* centroid_out = nullptr);
    float area() const;
    float inertia_per_mass() const;   // rotational inertia about the centroid divided by mass
    AABB bounds(Vec2 position, const Rot& rot) const;
    float max_extent() const;         // distance from centroid to furthest point (for CCD bounds)
    float min_extent() const;         // smallest half-width (for "fast body" detection)
};

struct BodyDef {
    BodyType type = BodyType::Dynamic;
    Shape shape = Shape::circle(0.5f);
    Vec2 position;
    float angle = 0;
    Vec2 velocity;
    float angular_velocity = 0;
    float density = 1.0f;          // mass = density * area unless mass > 0
    float mass = 0;                // explicit mass; <=0 means derive from density; INFINITY = immovable
    float friction = 0.6f;
    float restitution = 0.0f;
    float gravity_scale = 1.0f;
    float linear_damping = 0.0f;
    float angular_damping = 0.01f;
    bool fixed_rotation = false;
    bool bullet = false;           // always use continuous collision
    bool sensor = false;           // trigger: overlap events only, no collision response
    bool can_sleep = true;
    uint32_t category = 1;
    uint32_t mask = 0xFFFFFFFFu;
    uint32_t user = 0;             // owning EntityHandle bits
};

struct Body {
    BodyType type = BodyType::Dynamic;
    Shape shape;
    Vec2 position;
    float angle = 0;
    Rot rot;
    Vec2 velocity;
    float angular_velocity = 0;
    Vec2 force;
    float torque = 0;
    float mass = 0, inv_mass = 0, inertia = 0, inv_inertia = 0;
    float friction = 0.6f, restitution = 0, gravity_scale = 1;
    float linear_damping = 0, angular_damping = 0;
    bool fixed_rotation = false, bullet = false, sensor = false, can_sleep = true;
    bool awake = true;
    float sleep_time = 0;
    uint32_t category = 1, mask = 0xFFFFFFFFu;
    uint32_t user = 0;
    AABB aabb;
    // Pre-step pose, for CCD sweeps and failure rollback.
    Vec2 position0; float angle0 = 0; Vec2 velocity0; float angular_velocity0 = 0;
};

struct ManifoldPoint {
    Vec2 local_point;          // meaning depends on manifold type (Box2D convention)
    float normal_impulse = 0;
    float tangent_impulse = 0;
    uint32_t id = 0;           // feature key for warm starting
};

struct Manifold {
    enum Type : uint8_t { Circles, FaceA, FaceB } type = Circles;
    Vec2 local_normal;
    Vec2 local_point;
    ManifoldPoint points[2];
    int count = 0;
};

struct WorldManifold { Vec2 normal; Vec2 points[2]; float separations[2]; };

enum class ContactEventType : uint8_t { Begin, Persist, End, TriggerBegin, TriggerEnd };

struct ContactEvent {
    ContactEventType type;
    PhysicsBodyHandle a, b;
    uint32_t user_a = 0, user_b = 0;
    Vec2 normal;      // from a to b
    Vec2 point;
    float impulse = 0;
};

struct RayHit { PhysicsBodyHandle body; Vec2 point; Vec2 normal; float fraction = 1; };

struct PhysicsStats {
    uint32_t bodies = 0, awake = 0, pairs = 0, contacts = 0, touching = 0, ccd_hits = 0, failures = 0;
};

class PhysicsWorld {
public:
    PhysicsWorld();

    PhysicsBodyHandle create_body(const BodyDef& def);
    bool destroy_body(PhysicsBodyHandle h);
    bool valid(PhysicsBodyHandle h) const { return handles_.valid(h); }
    Body* get(PhysicsBodyHandle h) { return valid(h) ? &bodies_[h.index()] : nullptr; }
    const Body* get(PhysicsBodyHandle h) const { return valid(h) ? &bodies_[h.index()] : nullptr; }

    void apply_force(PhysicsBodyHandle h, Vec2 f);
    void apply_impulse(PhysicsBodyHandle h, Vec2 impulse);
    void set_velocity(PhysicsBodyHandle h, Vec2 v);
    void set_transform(PhysicsBodyHandle h, Vec2 position, float angle);
    void wake(PhysicsBodyHandle h);

    void step(float dt);

    const std::vector<ContactEvent>& events() const { return events_; }
    bool raycast(Vec2 from, Vec2 to, uint32_t mask, RayHit& out, PhysicsBodyHandle ignore = {}) const;
    /// All bodies whose shape overlaps the given AABB (broad test), in handle order.
    void query_aabb(const AABB& box, std::vector<PhysicsBodyHandle>& out) const;

    Vec2 gravity{0, -20.0f};
    int velocity_iterations = 8;
    int position_iterations = 3;
    PhysicsStats stats;

    uint32_t body_count() const { return handles_.alive(); }
    PhysicsBodyHandle handle_at(uint32_t index) const { return handles_.at(index); }

    void serialize(BinaryWriter& w) const;
    void deserialize(BinaryReader& r);
    void clear();

private:
    struct Contact {
        PhysicsBodyHandle a, b;
        Manifold manifold;
        bool touching = false;
        bool sensor = false;
        bool seen = false;     // refreshed this step
        float friction = 0, restitution = 0;
    };
    struct VelocityPoint { Vec2 ra, rb; float normal_mass, tangent_mass, velocity_bias; };
    struct VelocityConstraint { Contact* c; Vec2 normal; VelocityPoint p[2]; int count; };

    void update_aabbs();
    void broadphase();
    void narrowphase(Contact& c);
    void solve(float dt);
    void continuous(float dt);
    void update_sleep(float dt);
    void guard_failures();
    void set_mass(Body& b, const BodyDef& def);
    static uint64_t pair_key(PhysicsBodyHandle a, PhysicsBodyHandle b) {
        uint32_t x = a.bits, y = b.bits;
        if (x > y) std::swap(x, y);
        return (uint64_t(x) << 32) | y;
    }

    HandlePool<PhysicsBodyHandle> handles_;
    std::vector<Body> bodies_;
    std::vector<uint32_t> order_;          // SAP order (body slot indices), kept sorted by aabb.min.x
    // Ordered => deterministic solver order. Nodes come from a pool: no heap traffic per contact.
    std::map<uint64_t, Contact, std::less<uint64_t>, PoolStdAllocator<std::pair<const uint64_t, Contact>>> contacts_;
    std::vector<ContactEvent> events_;
    std::vector<VelocityConstraint> constraints_;
    std::vector<uint32_t> island_parent_;
    std::vector<float> island_min_;
    std::vector<uint8_t> was_touching_;
};

// Narrowphase and distance primitives (exposed for tests).
void collide(const Body& a, const Body& b, Manifold& m);
WorldManifold world_manifold(const Manifold& m, const Body& a, const Body& b);
/// GJK distance between two posed shapes, including their radii. Returns separation (<0 = overlap
/// is reported as 0) and closest points.
/// Distance between core shapes (vertices / circle centres) without radii; used by CCD.
float shape_core_distance(const Shape& sa, Vec2 pa, const Rot& ra, const Shape& sb, Vec2 pb, const Rot& rb);
float shape_distance(const Shape& sa, Vec2 pa, const Rot& ra, const Shape& sb, Vec2 pb, const Rot& rb, Vec2* point_a = nullptr, Vec2* point_b = nullptr);

}  // namespace unify
