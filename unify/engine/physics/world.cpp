#include "physics.h"
#include "../core/core.h"
#include "../serialization/stream.h"
#include <cfloat>

namespace unify {
using namespace phys;

PhysicsWorld::PhysicsWorld() {
    bodies_.reserve(1024);
    order_.reserve(1024);
    events_.reserve(512);
    constraints_.reserve(512);
}

void PhysicsWorld::set_mass(Body& b, const BodyDef& def) {
    b.mass = b.inv_mass = b.inertia = b.inv_inertia = 0;
    if (b.type != BodyType::Dynamic) return;
    float m;
    if (std::isinf(def.mass)) {           // "infinite mass": immovable dynamic body
        b.mass = INFINITY;
        return;
    } else if (def.mass > 0 && std::isfinite(def.mass)) {
        m = def.mass;
    } else {
        m = def.density * b.shape.area();
    }
    if (!(m > 0) || !std::isfinite(m)) {  // zero/negative/NaN mass: fall back to unit mass, never divide by 0
        UNIFY_LOG_WARN("PHYSICS", "body has non-positive mass (%g); using 1.0", double(m));
        m = 1.0f;
    }
    b.mass = m;
    b.inv_mass = 1.0f / m;
    b.inertia = m * b.shape.inertia_per_mass();
    bool rotation_locked = b.fixed_rotation || b.shape.type == ShapeType::AABB;
    b.inv_inertia = (rotation_locked || b.inertia <= 0) ? 0.0f : 1.0f / b.inertia;
}

PhysicsBodyHandle PhysicsWorld::create_body(const BodyDef& def) {
    PhysicsBodyHandle h = handles_.allocate();
    if (h.index() >= bodies_.size()) bodies_.resize(h.index() + 1);
    Body& b = bodies_[h.index()];
    b = Body{};
    b.type = def.type;
    b.shape = def.shape;
    b.position = def.position;
    b.angle = def.shape.type == ShapeType::AABB ? 0.0f : def.angle;
    b.rot = Rot(b.angle);
    b.velocity = def.velocity;
    b.angular_velocity = def.angular_velocity;
    b.friction = def.friction;
    b.restitution = def.restitution;
    b.gravity_scale = def.gravity_scale;
    b.linear_damping = def.linear_damping;
    b.angular_damping = def.angular_damping;
    b.fixed_rotation = def.fixed_rotation || def.shape.type == ShapeType::AABB;
    b.bullet = def.bullet;
    b.sensor = def.sensor;
    b.can_sleep = def.can_sleep;
    b.category = def.category;
    b.mask = def.mask;
    b.user = def.user;
    b.awake = def.type != BodyType::Static;
    set_mass(b, def);
    if (b.fixed_rotation) b.angular_velocity = 0;
    b.aabb = b.shape.bounds(b.position, b.rot);
    b.position0 = b.position;
    b.angle0 = b.angle;
    // Insert into SAP order at the right place so the order stays sorted and deterministic.
    auto it = order_.begin();
    while (it != order_.end() && bodies_[*it].aabb.min.x <= b.aabb.min.x) ++it;
    order_.insert(it, h.index());
    return h;
}

bool PhysicsWorld::destroy_body(PhysicsBodyHandle h) {
    if (!handles_.valid(h)) return false;
    // End any contacts so gameplay sees matching End events.
    for (auto it = contacts_.begin(); it != contacts_.end();) {
        if (it->second.a == h || it->second.b == h) {
            if (it->second.touching) {
                const Body& ba = bodies_[it->second.a.index()];
                const Body& bb = bodies_[it->second.b.index()];
                events_.push_back({it->second.sensor ? ContactEventType::TriggerEnd : ContactEventType::End,
                                   it->second.a, it->second.b, ba.user, bb.user, {}, {}, 0});
                // Wake the survivor so it does not float where the destroyed body was.
                PhysicsBodyHandle other = it->second.a == h ? it->second.b : it->second.a;
                if (bodies_[other.index()].type == BodyType::Dynamic) { bodies_[other.index()].awake = true; bodies_[other.index()].sleep_time = 0; }
            }
            it = contacts_.erase(it);
        } else {
            ++it;
        }
    }
    for (size_t i = 0; i < order_.size(); i++) if (order_[i] == h.index()) { order_.erase(order_.begin() + long(i)); break; }
    handles_.release(h);
    return true;
}

void PhysicsWorld::wake(PhysicsBodyHandle h) {
    if (Body* b = get(h)) if (b->type != BodyType::Static) { b->awake = true; b->sleep_time = 0; }
}
void PhysicsWorld::apply_force(PhysicsBodyHandle h, Vec2 f) {
    Body* b = get(h);
    if (!b || b->type != BodyType::Dynamic || !finite(f)) return;
    b->force += f;
    wake(h);
}
void PhysicsWorld::apply_impulse(PhysicsBodyHandle h, Vec2 impulse) {
    Body* b = get(h);
    if (!b || b->type != BodyType::Dynamic || !finite(impulse)) return;
    b->velocity += b->inv_mass * impulse;
    wake(h);
}
void PhysicsWorld::set_velocity(PhysicsBodyHandle h, Vec2 v) {
    Body* b = get(h);
    if (!b || b->type == BodyType::Static || !finite(v)) return;
    b->velocity = v;
    if (length_sq(v) > 0) wake(h);
}
void PhysicsWorld::set_transform(PhysicsBodyHandle h, Vec2 p, float angle) {
    Body* b = get(h);
    if (!b || !finite(p) || !std::isfinite(angle)) return;
    b->position = p;
    b->angle = b->fixed_rotation ? b->angle : angle;
    b->rot = Rot(b->angle);
    b->aabb = b->shape.bounds(b->position, b->rot);
    wake(h);
}

// ------------------------------------------------------------------------------ step

void PhysicsWorld::update_aabbs() {
    for (uint32_t i : order_) {
        Body& b = bodies_[i];
        b.aabb = b.shape.bounds(b.position, b.rot);
    }
    // Insertion sort: O(n) for the nearly-sorted order that frame coherence produces,
    // and stable, so equal keys keep a deterministic order.
    for (size_t i = 1; i < order_.size(); i++) {
        uint32_t key = order_[i];
        float x = bodies_[key].aabb.min.x;
        size_t j = i;
        while (j > 0 && bodies_[order_[j - 1]].aabb.min.x > x) { order_[j] = order_[j - 1]; j--; }
        order_[j] = key;
    }
}

static bool should_collide(const Body& a, const Body& b) {
    if ((a.category & b.mask) == 0 || (b.category & a.mask) == 0) return false;
    if (a.type != BodyType::Dynamic && b.type != BodyType::Dynamic) return a.sensor || b.sensor ? (a.type == BodyType::Kinematic || b.type == BodyType::Kinematic) : false;
    return true;
}

void PhysicsWorld::broadphase() {
    for (auto& kv : contacts_) kv.second.seen = false;
    stats.pairs = 0;
    for (size_t i = 0; i < order_.size(); i++) {
        const Body& a = bodies_[order_[i]];
        for (size_t j = i + 1; j < order_.size(); j++) {
            const Body& b = bodies_[order_[j]];
            if (b.aabb.min.x > a.aabb.max.x) break;
            if (a.aabb.max.y < b.aabb.min.y || b.aabb.max.y < a.aabb.min.y) continue;
            if (!should_collide(a, b)) continue;
            bool active_a = a.awake && a.type != BodyType::Static;
            bool active_b = b.awake && b.type != BodyType::Static;
            PhysicsBodyHandle ha = handles_.at(order_[i]), hb = handles_.at(order_[j]);
            uint64_t key = pair_key(ha, hb);
            auto it = contacts_.find(key);
            if (!active_a && !active_b) {  // both resting/static: keep the cached contact as-is
                if (it != contacts_.end()) it->second.seen = true;
                continue;
            }
            stats.pairs++;
            if (it == contacts_.end()) {
                Contact c;
                // Polygon-vs-circle manifolds are defined polygon-first.
                bool swap = a.shape.type == ShapeType::Circle && b.shape.type != ShapeType::Circle;
                c.a = swap ? hb : ha;
                c.b = swap ? ha : hb;
                c.sensor = a.sensor || b.sensor;
                c.friction = std::sqrt(a.friction * b.friction);
                c.restitution = std::max(a.restitution, b.restitution);
                it = contacts_.emplace(key, c).first;
            }
            it->second.seen = true;
            narrowphase(it->second);
        }
    }
}

void PhysicsWorld::narrowphase(Contact& c) {
    Manifold old = c.manifold;
    collide(bodies_[c.a.index()], bodies_[c.b.index()], c.manifold);
    // Warm start: carry impulses across steps for points with the same feature id.
    for (int i = 0; i < c.manifold.count; i++) {
        ManifoldPoint& p = c.manifold.points[i];
        p.normal_impulse = p.tangent_impulse = 0;
        for (int j = 0; j < old.count; j++)
            if (old.points[j].id == p.id) { p.normal_impulse = old.points[j].normal_impulse; p.tangent_impulse = old.points[j].tangent_impulse; break; }
    }
}

void PhysicsWorld::solve(float dt) {
    constraints_.clear();
    for (auto& kv : contacts_) {
        Contact& c = kv.second;
        if (c.sensor || c.manifold.count == 0) continue;
        Body& a = bodies_[c.a.index()];
        Body& b = bodies_[c.b.index()];
        if (!(a.awake && a.type == BodyType::Dynamic) && !(b.awake && b.type == BodyType::Dynamic)) continue;
        WorldManifold wm = world_manifold(c.manifold, a, b);
        VelocityConstraint vc{};
        vc.c = &c;
        vc.normal = wm.normal;
        vc.count = c.manifold.count;
        Vec2 tangent = cross(wm.normal, 1.0f);
        for (int i = 0; i < vc.count; i++) {
            VelocityPoint& p = vc.p[i];
            p.ra = wm.points[i] - a.position;
            p.rb = wm.points[i] - b.position;
            float rna = cross(p.ra, wm.normal), rnb = cross(p.rb, wm.normal);
            float kn = a.inv_mass + b.inv_mass + a.inv_inertia * rna * rna + b.inv_inertia * rnb * rnb;
            p.normal_mass = kn > 0 ? 1.0f / kn : 0;
            float rta = cross(p.ra, tangent), rtb = cross(p.rb, tangent);
            float kt = a.inv_mass + b.inv_mass + a.inv_inertia * rta * rta + b.inv_inertia * rtb * rtb;
            p.tangent_mass = kt > 0 ? 1.0f / kt : 0;
            Vec2 dv = b.velocity + cross(b.angular_velocity, p.rb) - a.velocity - cross(a.angular_velocity, p.ra);
            float vrel = dot(wm.normal, dv);
            p.velocity_bias = vrel < -kVelocityThreshold ? -c.restitution * vrel : 0;
        }
        constraints_.push_back(vc);
    }

    // Warm start.
    for (auto& vc : constraints_) {
        Body& a = bodies_[vc.c->a.index()];
        Body& b = bodies_[vc.c->b.index()];
        Vec2 tangent = cross(vc.normal, 1.0f);
        for (int i = 0; i < vc.count; i++) {
            const ManifoldPoint& mp = vc.c->manifold.points[i];
            Vec2 P = mp.normal_impulse * vc.normal + mp.tangent_impulse * tangent;
            a.velocity -= a.inv_mass * P; a.angular_velocity -= a.inv_inertia * cross(vc.p[i].ra, P);
            b.velocity += b.inv_mass * P; b.angular_velocity += b.inv_inertia * cross(vc.p[i].rb, P);
        }
    }

    for (int it = 0; it < velocity_iterations; it++) {
        for (auto& vc : constraints_) {
            Body& a = bodies_[vc.c->a.index()];
            Body& b = bodies_[vc.c->b.index()];
            Vec2 tangent = cross(vc.normal, 1.0f);
            float friction = vc.c->friction;
            // Friction first so the normal (non-penetration) solve has the last word.
            for (int i = 0; i < vc.count; i++) {
                VelocityPoint& p = vc.p[i];
                ManifoldPoint& mp = vc.c->manifold.points[i];
                Vec2 dv = b.velocity + cross(b.angular_velocity, p.rb) - a.velocity - cross(a.angular_velocity, p.ra);
                float lambda = p.tangent_mass * -dot(dv, tangent);
                float max_f = friction * mp.normal_impulse;
                float next = clampf(mp.tangent_impulse + lambda, -max_f, max_f);
                lambda = next - mp.tangent_impulse;
                mp.tangent_impulse = next;
                Vec2 P = lambda * tangent;
                a.velocity -= a.inv_mass * P; a.angular_velocity -= a.inv_inertia * cross(p.ra, P);
                b.velocity += b.inv_mass * P; b.angular_velocity += b.inv_inertia * cross(p.rb, P);
            }
            for (int i = 0; i < vc.count; i++) {
                VelocityPoint& p = vc.p[i];
                ManifoldPoint& mp = vc.c->manifold.points[i];
                Vec2 dv = b.velocity + cross(b.angular_velocity, p.rb) - a.velocity - cross(a.angular_velocity, p.ra);
                float vn = dot(dv, vc.normal);
                float lambda = -p.normal_mass * (vn - p.velocity_bias);
                float next = std::max(mp.normal_impulse + lambda, 0.0f);
                lambda = next - mp.normal_impulse;
                mp.normal_impulse = next;
                Vec2 P = lambda * vc.normal;
                a.velocity -= a.inv_mass * P; a.angular_velocity -= a.inv_inertia * cross(p.ra, P);
                b.velocity += b.inv_mass * P; b.angular_velocity += b.inv_inertia * cross(p.rb, P);
            }
        }
    }

    // Integrate positions (dynamic and kinematic), clamping absurd translations.
    for (uint32_t i : order_) {
        Body& b = bodies_[i];
        if (b.type == BodyType::Static || !b.awake) continue;
        Vec2 move = dt * b.velocity;
        float ls = length_sq(move);
        if (ls > kMaxTranslation * kMaxTranslation) b.velocity *= kMaxTranslation / std::sqrt(ls);
        b.position += dt * b.velocity;
        if (!b.fixed_rotation) { b.angle += dt * b.angular_velocity; b.rot = Rot(b.angle); }
    }

    // Position solver: push resolving penetration directly (no energy added to velocities).
    for (int it = 0; it < position_iterations; it++) {
        float min_sep = 0;
        for (auto& vc : constraints_) {
            Contact& c = *vc.c;
            Body& a = bodies_[c.a.index()];
            Body& b = bodies_[c.b.index()];
            const Manifold& m = c.manifold;
            float ma = a.inv_mass, mb = b.inv_mass, ia = a.inv_inertia, ib = b.inv_inertia;
            for (int j = 0; j < m.count; j++) {
                Vec2 normal, point;
                float sep;
                float ra = a.shape.radius, rb = b.shape.radius;
                if (m.type == Manifold::Circles) {
                    Vec2 pa = a.position + a.rot.apply(m.local_point), pb = b.position + b.rot.apply(m.points[0].local_point);
                    Vec2 d = pb - pa;
                    normal = length_sq(d) > FLT_EPSILON * FLT_EPSILON ? normalize(d) : Vec2{0, 1};
                    point = 0.5f * (pa + pb);
                    sep = dot(d, normal) - ra - rb;
                } else if (m.type == Manifold::FaceA) {
                    normal = a.rot.apply(m.local_normal);
                    Vec2 plane = a.position + a.rot.apply(m.local_point);
                    Vec2 clip = b.position + b.rot.apply(m.points[j].local_point);
                    sep = dot(clip - plane, normal) - ra - rb;
                    point = clip;
                } else {
                    normal = b.rot.apply(m.local_normal);
                    Vec2 plane = b.position + b.rot.apply(m.local_point);
                    Vec2 clip = a.position + a.rot.apply(m.points[j].local_point);
                    sep = dot(clip - plane, normal) - ra - rb;
                    point = clip;
                    normal = -normal;
                }
                Vec2 rA = point - a.position, rB = point - b.position;
                min_sep = std::min(min_sep, sep);
                float C = clampf(kBaumgarte * (sep + kLinearSlop), -kMaxLinearCorrection, 0.0f);
                float rna = cross(rA, normal), rnb = cross(rB, normal);
                float K = ma + mb + ia * rna * rna + ib * rnb * rnb;
                float impulse = K > 0 ? -C / K : 0;
                Vec2 P = impulse * normal;
                a.position -= ma * P;
                b.position += mb * P;
                if (ia > 0) { a.angle -= ia * cross(rA, P); a.rot = Rot(a.angle); }
                if (ib > 0) { b.angle += ib * cross(rB, P); b.rot = Rot(b.angle); }
            }
        }
        if (min_sep >= -3.0f * kLinearSlop) break;
    }
}

// Continuous collision: conservative advancement along each fast body's sweep against
// static/kinematic geometry (and everything, for bullets). Stops the body at first touch.
void PhysicsWorld::continuous(float) {
    for (uint32_t i : order_) {
        Body& b = bodies_[i];
        if (b.type != BodyType::Dynamic || !b.awake || b.sensor) continue;
        Vec2 motion = b.position - b.position0;
        float travel = length(motion) + std::fabs(b.angle - b.angle0) * b.shape.max_extent();
        if (!b.bullet && travel < 0.5f * b.shape.min_extent()) continue;

        Rot r0(b.angle0);
        // Swept bounds from the pre-step pose to the integrated pose (b.aabb is still pre-step here).
        AABB swept = b.shape.bounds(b.position0, r0).merged(b.shape.bounds(b.position, b.rot));
        float toi = 1.0f;
        for (uint32_t k : order_) {
            if (k == i) continue;
            const Body& o = bodies_[k];
            if (o.sensor || !should_collide(b, o)) continue;
            if (o.type == BodyType::Dynamic && !b.bullet) continue;
            AABB oswept = o.shape.bounds(o.position0, Rot(o.angle0)).merged(o.shape.bounds(o.position, o.rot));
            if (!swept.overlaps(oswept)) continue;

            // Measured on core shapes: stop where the skins overlap by ~3 slop, so the next
            // step's narrowphase is guaranteed a manifold and the solver removes the approach.
            const float target = std::max(kLinearSlop, b.shape.radius + o.shape.radius - 3.0f * kLinearSlop);
            const float tolerance = 0.25f * kLinearSlop;
            float d0 = shape_core_distance(b.shape, b.position0, r0, o.shape, o.position0, Rot(o.angle0));
            if (d0 <= target + tolerance) continue;  // already touching at the start: discrete contacts handle it
            float bound = length(motion - (o.position - o.position0)) + std::fabs(b.angle - b.angle0) * b.shape.max_extent() +
                          std::fabs(o.angle - o.angle0) * o.shape.max_extent();
            if (bound < 1e-9f) continue;
            float t = 0;
            for (int iter = 0; iter < 32; iter++) {
                Vec2 pb = lerp(b.position0, b.position, t), po = lerp(o.position0, o.position, t);
                float d = shape_core_distance(b.shape, pb, Rot(lerp(b.angle0, b.angle, t)), o.shape, po, Rot(lerp(o.angle0, o.angle, t)));
                if (d <= target + tolerance) break;
                t += (d - target) / bound;
                if (t >= toi) break;
            }
            if (t < toi) toi = t;
        }
        if (toi < 1.0f) {
            b.position = lerp(b.position0, b.position, toi);
            b.angle = lerp(b.angle0, b.angle, toi);
            b.rot = Rot(b.angle);
            b.aabb = b.shape.bounds(b.position, b.rot);
            stats.ccd_hits++;
        }
    }
}

void PhysicsWorld::update_sleep(float dt) {
    const float lin2 = kLinearSleepTolerance * kLinearSleepTolerance;
    const float ang2 = kAngularSleepTolerance * kAngularSleepTolerance;
    island_parent_.resize(bodies_.size());
    for (uint32_t i = 0; i < bodies_.size(); i++) island_parent_[i] = i;
    auto find = [&](uint32_t x) { while (island_parent_[x] != x) { island_parent_[x] = island_parent_[island_parent_[x]]; x = island_parent_[x]; } return x; };

    for (uint32_t i : order_) {
        Body& b = bodies_[i];
        if (b.type != BodyType::Dynamic || !b.awake) continue;
        if (!b.can_sleep || length_sq(b.velocity) > lin2 || b.angular_velocity * b.angular_velocity > ang2) b.sleep_time = 0;
        else b.sleep_time += dt;
    }
    for (auto& kv : contacts_) {
        const Contact& c = kv.second;
        if (!c.touching || c.sensor) continue;
        Body& a = bodies_[c.a.index()];
        Body& b = bodies_[c.b.index()];
        // A moving kinematic body keeps whatever it touches awake.
        if (a.type == BodyType::Kinematic && length_sq(a.velocity) > 0) b.sleep_time = 0;
        if (b.type == BodyType::Kinematic && length_sq(b.velocity) > 0) a.sleep_time = 0;
        if (a.type == BodyType::Dynamic && b.type == BodyType::Dynamic) {
            uint32_t ra = find(c.a.index()), rb = find(c.b.index());
            if (ra != rb) island_parent_[std::max(ra, rb)] = std::min(ra, rb);
        }
    }
    std::vector<float>& min_sleep = island_min_;
    min_sleep.assign(bodies_.size(), FLT_MAX);
    for (uint32_t i : order_) {
        const Body& b = bodies_[i];
        if (b.type != BodyType::Dynamic) continue;
        float t = b.awake ? b.sleep_time : kTimeToSleep;  // sleeping members count as ready
        uint32_t r = find(i);
        min_sleep[r] = std::min(min_sleep[r], t);
    }
    for (uint32_t i : order_) {
        Body& b = bodies_[i];
        if (b.type != BodyType::Dynamic) continue;
        if (min_sleep[find(i)] >= kTimeToSleep) {
            if (b.awake) { b.awake = false; b.velocity = {}; b.angular_velocity = 0; }
        } else if (!b.awake) {
            b.awake = true;
            b.sleep_time = 0;
        }
    }
}

void PhysicsWorld::guard_failures() {
    for (uint32_t i : order_) {
        Body& b = bodies_[i];
        if (finite(b.position) && std::isfinite(b.angle) && finite(b.velocity) && std::isfinite(b.angular_velocity)) continue;
        // Numeric failure (e.g. degenerate input). Roll the body back rather than letting NaN
        // propagate into transforms, contacts and the ECS.
        UNIFY_LOG_ERROR("PHYSICS", "non-finite state on body %u; rolled back", i);
        b.position = b.position0; b.angle = b.angle0; b.rot = Rot(b.angle);
        b.velocity = {}; b.angular_velocity = 0;
        b.force = {}; b.torque = 0;
        stats.failures++;
        for (auto& kv : contacts_)
            if (kv.second.a.index() == i || kv.second.b.index() == i)
                for (int k = 0; k < kv.second.manifold.count; k++) kv.second.manifold.points[k].normal_impulse = kv.second.manifold.points[k].tangent_impulse = 0;
    }
}

void PhysicsWorld::step(float dt) {
    events_.clear();
    if (!(dt > 0)) return;
    stats.ccd_hits = 0;

    for (uint32_t i : order_) {
        Body& b = bodies_[i];
        b.position0 = b.position; b.angle0 = b.angle;
        b.velocity0 = b.velocity; b.angular_velocity0 = b.angular_velocity;
        if (b.type != BodyType::Dynamic || !b.awake) continue;
        b.velocity += dt * (b.gravity_scale * gravity + b.inv_mass * b.force);
        b.angular_velocity += dt * b.inv_inertia * b.torque;
        b.velocity *= 1.0f / (1.0f + dt * b.linear_damping);
        b.angular_velocity *= 1.0f / (1.0f + dt * b.angular_damping);
    }

    update_aabbs();
    broadphase();

    // Touching state before the solve, so impulses from this step accompany the events.
    std::vector<uint8_t>& was = was_touching_;
    was.clear();
    for (auto& kv : contacts_) was.push_back(kv.second.touching ? 1 : 0);
    {
        for (auto& kv : contacts_) {
            Contact& c = kv.second;
            if (c.seen) {
                const Body& a = bodies_[c.a.index()];
                const Body& b = bodies_[c.b.index()];
                bool active = (a.awake && a.type != BodyType::Static) || (b.awake && b.type != BodyType::Static);
                if (active) c.touching = c.manifold.count > 0;
            } else {
                c.touching = false;
            }
        }
    }

    solve(dt);
    continuous(dt);
    guard_failures();
    update_sleep(dt);

    for (uint32_t i : order_) { bodies_[i].force = {}; bodies_[i].torque = 0; }

    // Events in deterministic (pair-key) order; drop contacts whose AABBs no longer overlap.
    size_t k = 0;
    stats.contacts = stats.touching = 0;
    for (auto it = contacts_.begin(); it != contacts_.end(); k++) {
        Contact& c = it->second;
        bool before = was[k] != 0;
        const Body& a = bodies_[c.a.index()];
        const Body& b = bodies_[c.b.index()];
        ContactEvent ev{};
        ev.a = c.a; ev.b = c.b; ev.user_a = a.user; ev.user_b = b.user;
        if (c.touching) {
            WorldManifold wm = world_manifold(c.manifold, a, b);
            ev.normal = wm.normal;
            ev.point = wm.points[0];
            for (int i = 0; i < c.manifold.count; i++) ev.impulse += c.manifold.points[i].normal_impulse;
        }
        bool asleep = !(a.awake && a.type != BodyType::Static) && !(b.awake && b.type != BodyType::Static);
        if (c.touching && !before) { ev.type = c.sensor ? ContactEventType::TriggerBegin : ContactEventType::Begin; events_.push_back(ev); }
        else if (!c.touching && before) { ev.type = c.sensor ? ContactEventType::TriggerEnd : ContactEventType::End; events_.push_back(ev); }
        else if (c.touching && !c.sensor && !asleep) { ev.type = ContactEventType::Persist; events_.push_back(ev); }
        if (!c.seen) { it = contacts_.erase(it); continue; }
        stats.contacts++;
        if (c.touching) stats.touching++;
        ++it;
    }

    stats.bodies = handles_.alive();
    stats.awake = 0;
    for (uint32_t i : order_) if (bodies_[i].awake && bodies_[i].type == BodyType::Dynamic) stats.awake++;
}

// ------------------------------------------------------------------------------ queries

static bool ray_shape(const Body& b, Vec2 p1, Vec2 p2, float& fraction, Vec2& normal) {
    Vec2 d = p2 - p1;
    if (b.shape.type == ShapeType::Circle) {
        Vec2 s = p1 - b.position;
        float r = b.shape.radius;
        float bb = dot(s, d), cc = dot(s, s) - r * r, rr = dot(d, d);
        float sigma = bb * bb - rr * cc;
        if (sigma < 0 || rr < FLT_EPSILON) return false;
        float a = -(bb + std::sqrt(sigma));
        if (a < 0 || a > rr) return false;
        fraction = a / rr;
        normal = normalize(s + fraction * d);
        return true;
    }
    Vec2 lp1 = b.rot.apply_inv(p1 - b.position), lp2 = b.rot.apply_inv(p2 - b.position), ld = lp2 - lp1;
    float lower = 0, upper = 1;
    int index = -1;
    for (int i = 0; i < b.shape.count; i++) {
        // Expand faces by the polygon skin so rays hit where contacts happen.
        float num = dot(b.shape.normals[i], b.shape.vertices[i] - lp1) + b.shape.radius;
        float den = dot(b.shape.normals[i], ld);
        if (den == 0) { if (num < 0) return false; continue; }
        if (den < 0 && num < lower * den) { lower = num / den; index = i; }
        else if (den > 0 && num < upper * den) { upper = num / den; }
        if (upper < lower) return false;
    }
    if (index < 0) return false;
    fraction = lower;
    normal = b.rot.apply(b.shape.normals[index]);
    return true;
}

bool PhysicsWorld::raycast(Vec2 from, Vec2 to, uint32_t mask, RayHit& out, PhysicsBodyHandle ignore) const {
    bool hit = false;
    out.fraction = 1;
    for (uint32_t i : order_) {
        const Body& b = bodies_[i];
        PhysicsBodyHandle h = handles_.at(i);
        if (h == ignore || b.sensor || (b.category & mask) == 0) continue;
        float f; Vec2 n;
        if (ray_shape(b, from, to, f, n) && f < out.fraction) {
            out = {h, from + f * (to - from), n, f};
            hit = true;
        }
    }
    return hit;
}

void PhysicsWorld::query_aabb(const AABB& box, std::vector<PhysicsBodyHandle>& out) const {
    out.clear();
    for (uint32_t i : order_) if (bodies_[i].aabb.overlaps(box)) out.push_back(handles_.at(i));
    std::sort(out.begin(), out.end());
}

// ------------------------------------------------------------------------------ save state

static void write_shape(BinaryWriter& w, const Shape& s) {
    w.u8(uint8_t(s.type)); w.f32(s.radius); w.u8(uint8_t(s.count));
    for (int i = 0; i < s.count; i++) { w.vec2(s.vertices[i]); w.vec2(s.normals[i]); }
}
static Shape read_shape(BinaryReader& r) {
    Shape s;
    s.type = ShapeType(r.u8()); s.radius = r.f32(); s.count = r.u8();
    if (s.count > kMaxPolygonVertices) throw SerializationError("polygon vertex count out of range");
    for (int i = 0; i < s.count; i++) { s.vertices[i] = r.vec2(); s.normals[i] = r.vec2(); }
    return s;
}

void PhysicsWorld::serialize(BinaryWriter& w) const {
    auto hs = handles_.save();
    w.u32(uint32_t(hs.gens.size()));
    for (size_t i = 0; i < hs.gens.size(); i++) { w.u8(hs.gens[i]); w.u8(hs.live[i]); }
    w.u32(uint32_t(hs.free.size()));
    for (uint32_t f : hs.free) w.u32(f);
    w.vec2(gravity);
    w.u32(uint32_t(order_.size()));
    for (uint32_t i : order_) {
        const Body& b = bodies_[i];
        w.u32(i);
        w.u8(uint8_t(b.type)); write_shape(w, b.shape);
        w.vec2(b.position); w.f32(b.angle); w.vec2(b.velocity); w.f32(b.angular_velocity);
        w.vec2(b.force); w.f32(b.torque);
        w.f32(b.mass); w.f32(b.inv_mass); w.f32(b.inertia); w.f32(b.inv_inertia);
        w.f32(b.friction); w.f32(b.restitution); w.f32(b.gravity_scale); w.f32(b.linear_damping); w.f32(b.angular_damping);
        w.u8(uint8_t(b.fixed_rotation | b.bullet << 1 | b.sensor << 2 | b.can_sleep << 3 | b.awake << 4));
        w.f32(b.sleep_time); w.u32(b.category); w.u32(b.mask); w.u32(b.user);
    }
    // Contacts carry warm-starting impulses; without them a restored world diverges.
    w.u32(uint32_t(contacts_.size()));
    for (auto& kv : contacts_) {
        const Contact& c = kv.second;
        w.u64(kv.first); w.u32(c.a.bits); w.u32(c.b.bits);
        w.u8(uint8_t(c.touching | c.sensor << 1)); w.f32(c.friction); w.f32(c.restitution);
        const Manifold& m = c.manifold;
        w.u8(uint8_t(m.type)); w.vec2(m.local_normal); w.vec2(m.local_point); w.u8(uint8_t(m.count));
        for (int i = 0; i < m.count; i++) { w.vec2(m.points[i].local_point); w.f32(m.points[i].normal_impulse); w.f32(m.points[i].tangent_impulse); w.u32(m.points[i].id); }
    }
}

void PhysicsWorld::deserialize(BinaryReader& r) {
    clear();
    HandlePool<PhysicsBodyHandle>::State hs;
    uint32_t n = r.u32();
    hs.gens.resize(n); hs.live.resize(n);
    for (uint32_t i = 0; i < n; i++) { hs.gens[i] = r.u8(); hs.live[i] = r.u8(); if (hs.live[i]) hs.alive++; }
    uint32_t nf = r.u32();
    hs.free.resize(nf);
    for (auto& f : hs.free) f = r.u32();
    handles_.load(hs);
    bodies_.assign(n, Body{});
    gravity = r.vec2();
    uint32_t count = r.u32();
    for (uint32_t k = 0; k < count; k++) {
        uint32_t i = r.u32();
        if (i >= n) throw SerializationError("body index out of range");
        Body& b = bodies_[i];
        b.type = BodyType(r.u8()); b.shape = read_shape(r);
        b.position = r.vec2(); b.angle = r.f32(); b.velocity = r.vec2(); b.angular_velocity = r.f32();
        b.force = r.vec2(); b.torque = r.f32();
        b.mass = r.f32(); b.inv_mass = r.f32(); b.inertia = r.f32(); b.inv_inertia = r.f32();
        b.friction = r.f32(); b.restitution = r.f32(); b.gravity_scale = r.f32(); b.linear_damping = r.f32(); b.angular_damping = r.f32();
        uint8_t f = r.u8();
        b.fixed_rotation = f & 1; b.bullet = f & 2; b.sensor = f & 4; b.can_sleep = f & 8; b.awake = f & 16;
        b.sleep_time = r.f32(); b.category = r.u32(); b.mask = r.u32(); b.user = r.u32();
        b.rot = Rot(b.angle);
        b.aabb = b.shape.bounds(b.position, b.rot);
        b.position0 = b.position; b.angle0 = b.angle;
        order_.push_back(i);
    }
    uint32_t nc = r.u32();
    for (uint32_t k = 0; k < nc; k++) {
        uint64_t key = r.u64();
        Contact c;
        c.a.bits = r.u32(); c.b.bits = r.u32();
        uint8_t f = r.u8(); c.touching = f & 1; c.sensor = f & 2;
        c.friction = r.f32(); c.restitution = r.f32();
        Manifold& m = c.manifold;
        m.type = Manifold::Type(r.u8()); m.local_normal = r.vec2(); m.local_point = r.vec2(); m.count = r.u8();
        if (m.count > 2) throw SerializationError("manifold point count out of range");
        for (int i = 0; i < m.count; i++) { m.points[i].local_point = r.vec2(); m.points[i].normal_impulse = r.f32(); m.points[i].tangent_impulse = r.f32(); m.points[i].id = r.u32(); }
        if (!handles_.valid(c.a) || !handles_.valid(c.b)) throw SerializationError("contact references a dead body");
        contacts_.emplace(key, c);
    }
}

void PhysicsWorld::clear() {
    handles_.clear();
    bodies_.clear();
    order_.clear();
    contacts_.clear();
    events_.clear();
    constraints_.clear();
    stats = {};
}

}  // namespace unify
