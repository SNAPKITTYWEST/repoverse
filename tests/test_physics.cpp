#include "test_main.h"
#include "../engine/physics/physics.h"
#include "../engine/serialization/stream.h"
#include "../engine/core/core.h"
#include <cstring>

using namespace unify;
static const float dt = 1.0f / 60.0f;

static PhysicsBodyHandle ground(PhysicsWorld& w, float half_w = 50, float top = 0, float half_h = 0.5f) {
    BodyDef d; d.type = BodyType::Static; d.shape = Shape::aabb(half_w, half_h); d.position = {0, top - half_h};
    return w.create_body(d);
}
static PhysicsBodyHandle ball(PhysicsWorld& w, Vec2 p, float r, Vec2 v = {}, bool bullet = false) {
    BodyDef d; d.shape = Shape::circle(r); d.position = p; d.velocity = v; d.bullet = bullet;
    return w.create_body(d);
}
static PhysicsBodyHandle box(PhysicsWorld& w, Vec2 p, float hw, float hh, BodyType t = BodyType::Dynamic, float angle = 0) {
    BodyDef d; d.type = t; d.shape = Shape::box(hw, hh); d.position = p; d.angle = angle;
    return w.create_body(d);
}
static void run(PhysicsWorld& w, int steps) { for (int i = 0; i < steps; i++) w.step(dt); }

TEST(physics_resting_contact_and_sleep) {
    PhysicsWorld w; ground(w);
    auto b = ball(w, {0, 3}, 0.5f);
    run(w, 240);
    const Body* body = w.get(b);
    CHECK_NEAR(body->position.y, 0.5f, 3 * phys::kLinearSlop + phys::kPolygonRadius);
    CHECK(length(body->velocity) < 0.05f);
    CHECK(!body->awake);  // came to rest and slept
}

TEST(physics_box_stack_is_stable) {
    PhysicsWorld w; ground(w);
    std::vector<PhysicsBodyHandle> stack;
    for (int i = 0; i < 6; i++) stack.push_back(box(w, {0, 0.5f + i * 1.0f}, 0.5f, 0.5f));
    run(w, 600);
    for (int i = 0; i < 6; i++) {
        const Body* b = w.get(stack[size_t(i)]);
        CHECK(std::fabs(b->position.x) < 0.05f);
        CHECK_NEAR(b->position.y, 0.5f + i * 1.0f, 0.1f);
        CHECK(std::fabs(b->angle) < 0.02f);
    }
    CHECK(w.stats.awake == 0);  // whole island asleep
}

TEST(physics_bullet_does_not_tunnel_thin_wall) {
    PhysicsWorld w;
    w.gravity = {0, 0};
    BodyDef wall; wall.type = BodyType::Static; wall.shape = Shape::aabb(0.02f, 5); wall.position = {10, 0};
    w.create_body(wall);
    auto b = ball(w, {0, 0}, 0.05f, {600, 0}, true);  // 10 m per step vs a 4 cm wall
    run(w, 10);
    CHECK(w.get(b)->position.x < 10.0f);
    CHECK(w.get(b)->position.x > 9.5f);
}

TEST(physics_fast_body_gets_ccd_without_bullet_flag) {
    PhysicsWorld w;
    auto g = ground(w, 50, 0, 0.05f);
    (void)g;
    auto b = box(w, {0, 20}, 0.25f, 0.25f);
    w.set_velocity(b, {0, -400});  // ~6.7 m per step, floor is 0.1 m thick
    run(w, 5);
    CHECK(w.get(b)->position.y > 0.0f);
    CHECK(w.get(b)->position.y < 1.0f);
}

TEST(physics_coincident_centers_separate_deterministically) {
    PhysicsWorld w; w.gravity = {0, 0};
    auto a = ball(w, {0, 0}, 0.5f), b = ball(w, {0, 0}, 0.5f);
    run(w, 60);
    Vec2 pa = w.get(a)->position, pb = w.get(b)->position;
    CHECK(finite(pa) && finite(pb));
    CHECK(length(pb - pa) > 0.9f);
    CHECK(w.stats.failures == 0);
}

TEST(physics_zero_mass_falls_back_to_unit_mass) {
    PhysicsWorld w; ground(w);
    BodyDef d; d.shape = Shape::box(0.5f, 0.5f); d.position = {0, 2}; d.density = 0;
    auto h = w.create_body(d);
    CHECK_NEAR(w.get(h)->mass, 1.0f, 0);
    run(w, 120);
    CHECK(finite(w.get(h)->position));
    CHECK_NEAR(w.get(h)->position.y, 0.5f, 0.05f);
}

TEST(physics_infinite_mass_is_immovable) {
    PhysicsWorld w; w.gravity = {0, 0};
    BodyDef d; d.shape = Shape::box(1, 1); d.position = {0, 0}; d.mass = INFINITY;
    auto wall = w.create_body(d);
    BodyDef bd; bd.shape = Shape::circle(0.25f); bd.position = {-3, 0}; bd.velocity = {10, 0}; bd.restitution = 0.5f;
    auto b = w.create_body(bd);
    run(w, 60);
    CHECK_NEAR(w.get(wall)->position.x, 0, 1e-6);
    CHECK(w.get(b)->velocity.x < 0);  // bounced off
}

TEST(physics_tiny_and_huge_objects) {
    PhysicsWorld w;
    BodyDef g; g.type = BodyType::Static; g.shape = Shape::aabb(2000, 0.5f); g.position = {0, -0.5f};
    w.create_body(g);
    auto tiny = ball(w, {0, 1}, 0.01f);
    auto huge = box(w, {500, 120}, 100, 100);
    run(w, 600);
    CHECK_NEAR(w.get(tiny)->position.y, 0.01f, 0.03f);
    CHECK_NEAR(w.get(huge)->position.y, 100.0f, 0.1f);
}

TEST(physics_parallel_edges_give_stable_two_point_manifold) {
    PhysicsWorld w; ground(w);
    auto b = box(w, {0, 0.5f}, 0.5f, 0.5f);
    run(w, 2);
    Manifold m;
    collide(*w.get(w.handle_at(0)), *w.get(b), m);
    CHECK(m.count == 2);
    uint32_t ids[2] = {m.points[0].id, m.points[1].id};
    run(w, 30);
    collide(*w.get(w.handle_at(0)), *w.get(b), m);
    CHECK(m.count == 2 && m.points[0].id == ids[0] && m.points[1].id == ids[1]);  // no reference-face flip-flop
}

TEST(physics_corner_and_edge_contacts) {
    Body ground_b; ground_b.shape = Shape::box(5, 0.5f); ground_b.position = {0, -0.5f};
    Body corner; corner.shape = Shape::box(0.5f, 0.5f); corner.angle = 0.785398f; corner.rot = Rot(corner.angle);
    corner.position = {0, 0.70f};
    Manifold m;
    collide(ground_b, corner, m);
    CHECK(m.count == 1);  // diamond resting on its corner
    Body circle; circle.shape = Shape::circle(0.5f);
    circle.position = {5.2f, 0.2f};  // beside the top-right corner: vertex region
    collide(ground_b, circle, m);
    CHECK(m.count == 1);
    WorldManifold wm = world_manifold(m, ground_b, circle);
    CHECK(wm.normal.x > 0.5f && wm.normal.y > 0.1f);
    circle.position = {1, 0.45f};  // above the face: edge region
    collide(ground_b, circle, m);
    wm = world_manifold(m, ground_b, circle);
    CHECK_NEAR(wm.normal.y, 1, 1e-5);
}

TEST(physics_multiple_contacts_across_platforms) {
    PhysicsWorld w;
    box(w, {-1.2f, -0.5f}, 1, 0.5f, BodyType::Static);
    box(w, {1.2f, -0.5f}, 1, 0.5f, BodyType::Static);
    auto plank = box(w, {0, 0.25f}, 2, 0.25f);
    run(w, 240);
    CHECK_NEAR(w.get(plank)->position.y, 0.25f, 0.05f);
    CHECK(std::fabs(w.get(plank)->angle) < 0.01f);
}

TEST(physics_trigger_events_without_response) {
    PhysicsWorld w; w.gravity = {0, 0};
    BodyDef s; s.type = BodyType::Static; s.shape = Shape::aabb(0.5f, 0.5f); s.position = {5, 0}; s.sensor = true; s.user = 77;
    w.create_body(s);
    auto b = ball(w, {0, 0}, 0.25f, {10, 0});
    int begin = 0, end = 0;
    for (int i = 0; i < 90; i++) {
        w.step(dt);
        for (auto& e : w.events()) {
            if (e.type == ContactEventType::TriggerBegin) begin++;
            if (e.type == ContactEventType::TriggerEnd) end++;
            CHECK(e.type != ContactEventType::Begin);
        }
    }
    CHECK(begin == 1 && end == 1);
    CHECK_NEAR(w.get(b)->velocity.x, 10, 1e-4);  // passed straight through
}

TEST(physics_collision_filtering) {
    PhysicsWorld w;
    BodyDef g; g.type = BodyType::Static; g.shape = Shape::aabb(10, 0.5f); g.position = {0, -0.5f}; g.category = 2;
    w.create_body(g);
    BodyDef d; d.shape = Shape::circle(0.5f); d.position = {0, 2}; d.mask = ~2u;  // ignores category 2
    auto b = w.create_body(d);
    run(w, 60);
    CHECK(w.get(b)->position.y < -1.0f);  // fell through the filtered ground
}

TEST(physics_contact_begin_persist_end) {
    PhysicsWorld w; ground(w);
    auto b = ball(w, {0, 1}, 0.5f);
    int begin = 0, persist = 0, end = 0;
    for (int i = 0; i < 60; i++) { w.step(dt); for (auto& e : w.events()) { begin += e.type == ContactEventType::Begin; persist += e.type == ContactEventType::Persist; } }
    w.set_velocity(b, {0, 12});
    for (int i = 0; i < 10; i++) { w.step(dt); for (auto& e : w.events()) end += e.type == ContactEventType::End; }
    CHECK(begin == 1);
    CHECK(persist > 10);
    CHECK(end == 1);
}

TEST(physics_sleeping_island_wakes_on_impulse) {
    PhysicsWorld w; ground(w);
    auto bottom = box(w, {0, 0.5f}, 0.5f, 0.5f);
    auto top = box(w, {0, 1.5f}, 0.5f, 0.5f);
    run(w, 240);
    CHECK(!w.get(bottom)->awake && !w.get(top)->awake);
    w.apply_impulse(bottom, {3, 0});
    run(w, 1);
    CHECK(w.get(bottom)->awake);
    CHECK(w.get(top)->awake);  // woken through the island
}

TEST(physics_nan_is_contained) {
    PhysicsWorld w; ground(w);
    auto good = ball(w, {2, 1}, 0.5f);
    auto bad = ball(w, {0, 1}, 0.5f);
    run(w, 10);
    Vec2 before = w.get(bad)->position;
    w.get(bad)->velocity = {NAN, 0};  // simulate corrupted input
    w.step(dt);
    CHECK(w.stats.failures == 1);
    CHECK(finite(w.get(bad)->position));
    CHECK(length(w.get(bad)->position - before) < 0.1f);
    CHECK(finite(w.get(good)->position));
}

TEST(physics_deep_penetration_resolves_gently) {
    PhysicsWorld w; ground(w);
    auto b = box(w, {0, 0.2f}, 0.5f, 0.5f);  // 0.3 m inside the ground
    float max_speed = 0;
    for (int i = 0; i < 120; i++) { w.step(dt); max_speed = std::max(max_speed, length(w.get(b)->velocity)); }
    CHECK_NEAR(w.get(b)->position.y, 0.5f, 0.03f);
    CHECK(max_speed < 3.0f);  // position solver, not an explosive velocity kick
}

TEST(physics_zero_dt_and_zero_velocity) {
    PhysicsWorld w; ground(w);
    auto b = box(w, {0, 0.5f}, 0.5f, 0.5f);
    w.step(0);
    CHECK(w.get(b)->position == Vec2(0, 0.5f));
    run(w, 60);
    CHECK(std::fabs(w.get(b)->position.x) < 1e-4f);
}

TEST(physics_restitution_bounces) {
    PhysicsWorld w; ground(w);
    BodyDef d; d.shape = Shape::circle(0.25f); d.position = {0, 3}; d.restitution = 0.8f;
    auto b = w.create_body(d);
    float peak_after = 0;
    bool bounced = false;
    for (int i = 0; i < 180; i++) {
        w.step(dt);
        if (w.get(b)->velocity.y > 0.5f) bounced = true;
        if (bounced) peak_after = std::max(peak_after, w.get(b)->position.y);
    }
    CHECK(bounced);
    CHECK(peak_after > 1.2f);
}

TEST(physics_raycast_hits_nearest) {
    PhysicsWorld w; w.gravity = {0, 0};
    ground(w);
    auto near_box = box(w, {0, 2}, 0.5f, 0.5f);
    RayHit hit;
    CHECK(w.raycast({0, 5}, {0, -5}, 0xFFFFFFFFu, hit));
    CHECK(hit.body == near_box);
    CHECK_NEAR(hit.point.y, 2.5f + phys::kPolygonRadius, 1e-3);
    CHECK_NEAR(hit.normal.y, 1, 1e-5);
}

static uint64_t world_hash(PhysicsWorld& w) {
    BinaryWriter bw;
    w.serialize(bw);
    return fnv1a(bw.data().data(), bw.size());
}

TEST(physics_is_deterministic_and_save_restore_is_exact) {
    auto build = [](PhysicsWorld& w) {
        ground(w);
        for (int i = 0; i < 8; i++) box(w, {float(i % 3) * 0.3f, 1 + i * 1.1f}, 0.4f, 0.3f, BodyType::Dynamic, 0.1f * i);
        for (int i = 0; i < 5; i++) ball(w, {2 + i * 0.2f, 3.0f + float(i)}, 0.3f);
    };
    PhysicsWorld a, b;
    build(a); build(b);
    run(a, 100); run(b, 100);
    CHECK(world_hash(a) == world_hash(b));

    BinaryWriter snap;
    a.serialize(snap);
    PhysicsWorld c;
    BinaryReader r(snap.data());
    c.deserialize(r);
    CHECK(world_hash(c) == world_hash(a));
    run(a, 200); run(c, 200);
    CHECK(world_hash(a) == world_hash(c));  // continues bit-identically after restore
}
