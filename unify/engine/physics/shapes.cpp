// Shape construction, mass properties, narrowphase manifolds and GJK distance.
// The manifold/solver conventions follow the well-known Box2D v2 formulation
// (reference face + clipping, local manifold points for the position solver).
#include "physics.h"
#include <cfloat>

namespace unify {
using namespace phys;

// ------------------------------------------------------------------------------ shapes

Shape Shape::circle(float radius) {
    Shape s;
    s.type = ShapeType::Circle;
    s.radius = std::max(radius, 0.25f * kLinearSlop);
    return s;
}

static void box_vertices(Shape& s, float hw, float hh) {
    s.count = 4;
    s.vertices[0] = {-hw, -hh}; s.vertices[1] = {hw, -hh}; s.vertices[2] = {hw, hh}; s.vertices[3] = {-hw, hh};
    s.normals[0] = {0, -1}; s.normals[1] = {1, 0}; s.normals[2] = {0, 1}; s.normals[3] = {-1, 0};
    s.radius = kPolygonRadius;
}

Shape Shape::aabb(float hw, float hh) {
    Shape s;
    s.type = ShapeType::AABB;
    box_vertices(s, std::max(hw, kLinearSlop), std::max(hh, kLinearSlop));
    return s;
}

Shape Shape::box(float hw, float hh) {
    Shape s;
    s.type = ShapeType::Box;
    box_vertices(s, std::max(hw, kLinearSlop), std::max(hh, kLinearSlop));
    return s;
}

Shape Shape::polygon(const Vec2* points, int n, Vec2* centroid_out) {
    // Weld near-duplicate points, then gift-wrap the convex hull (counter-clockwise).
    Vec2 ps[kMaxPolygonVertices];
    int m = 0;
    for (int i = 0; i < n && m < kMaxPolygonVertices; i++) {
        bool unique = true;
        for (int j = 0; j < m; j++) if (length_sq(points[i] - ps[j]) < 0.25f * kLinearSlop * kLinearSlop) { unique = false; break; }
        if (unique) ps[m++] = points[i];
    }
    if (m < 3) return Shape::box(0.5f, 0.5f);

    int i0 = 0;
    for (int i = 1; i < m; i++)
        if (ps[i].x > ps[i0].x || (ps[i].x == ps[i0].x && ps[i].y < ps[i0].y)) i0 = i;
    int hull[kMaxPolygonVertices];
    int hn = 0, ih = i0;
    for (;;) {
        hull[hn] = ih;
        int ie = 0;
        for (int j = 1; j < m; j++) {
            if (ie == ih) { ie = j; continue; }
            Vec2 r = ps[ie] - ps[hull[hn]], v = ps[j] - ps[hull[hn]];
            float c = cross(r, v);
            if (c < 0 || (c == 0 && length_sq(v) > length_sq(r))) ie = j;
        }
        hn++;
        ih = ie;
        if (ie == i0 || hn == kMaxPolygonVertices) break;
    }
    if (hn < 3) return Shape::box(0.5f, 0.5f);

    Shape s;
    s.type = ShapeType::Polygon;
    s.count = hn;
    for (int i = 0; i < hn; i++) s.vertices[i] = ps[hull[i]];

    // Centroid by triangle fan.
    Vec2 c{0, 0};
    float area = 0;
    Vec2 ref = s.vertices[0];
    for (int i = 1; i + 1 < hn; i++) {
        Vec2 e1 = s.vertices[i] - ref, e2 = s.vertices[i + 1] - ref;
        float a = 0.5f * cross(e1, e2);
        area += a;
        c += a * (1.0f / 3.0f) * (ref + s.vertices[i] + ref + s.vertices[i + 1] - ref);
    }
    c = area > 1e-12f ? c * (1.0f / area) : ref;
    for (int i = 0; i < hn; i++) s.vertices[i] -= c;
    for (int i = 0; i < hn; i++) {
        Vec2 e = s.vertices[(i + 1) % hn] - s.vertices[i];
        s.normals[i] = normalize(Vec2{e.y, -e.x});
    }
    s.radius = kPolygonRadius;
    if (centroid_out) *centroid_out = c;
    return s;
}

float Shape::area() const {
    if (type == ShapeType::Circle) return 3.14159265f * radius * radius;
    float a = 0;
    for (int i = 1; i + 1 < count; i++) a += 0.5f * cross(vertices[i] - vertices[0], vertices[i + 1] - vertices[0]);
    return a;
}

float Shape::inertia_per_mass() const {
    if (type == ShapeType::Circle) return 0.5f * radius * radius;
    // Sum of triangle (origin, v_i, v_i+1) inertias about the origin (= centroid), per unit density.
    float inertia = 0, area_sum = 0;
    for (int i = 0; i < count; i++) {
        Vec2 e1 = vertices[i], e2 = vertices[(i + 1) % count];
        float d = cross(e1, e2);
        area_sum += 0.5f * d;
        inertia += (d / 12.0f) * (dot(e1, e1) + dot(e1, e2) + dot(e2, e2));
    }
    return area_sum > 0 ? inertia / area_sum : 0;
}

AABB Shape::bounds(Vec2 p, const Rot& r) const {
    if (type == ShapeType::Circle) return {{p.x - radius, p.y - radius}, {p.x + radius, p.y + radius}};
    Vec2 lo = p + r.apply(vertices[0]), hi = lo;
    for (int i = 1; i < count; i++) { Vec2 v = p + r.apply(vertices[i]); lo = vmin(lo, v); hi = vmax(hi, v); }
    return AABB{lo, hi}.expanded(radius);
}

float Shape::max_extent() const {
    if (type == ShapeType::Circle) return radius;
    float m = 0;
    for (int i = 0; i < count; i++) m = std::max(m, length(vertices[i]));
    return m + radius;
}

float Shape::min_extent() const {
    if (type == ShapeType::Circle) return radius;
    float m = FLT_MAX;
    for (int i = 0; i < count; i++) m = std::min(m, -dot(normals[i], vertices[i]) * -1.0f);
    return m + radius;
}

// ------------------------------------------------------------------------------ narrowphase

namespace {
struct Xf { Vec2 p; Rot q; };
Vec2 mul(const Xf& x, Vec2 v) { return x.p + x.q.apply(v); }
Vec2 mul_t(const Xf& x, Vec2 v) { return x.q.apply_inv(v - x.p); }
Xf xf_of(const Body& b) { return {b.position, b.rot}; }

struct ClipVertex { Vec2 v; uint32_t id; };
uint32_t feature(uint8_t ia, uint8_t ib, uint8_t ta, uint8_t tb) { return uint32_t(ia) | uint32_t(ib) << 8 | uint32_t(ta) << 16 | uint32_t(tb) << 24; }
constexpr uint8_t kVertex = 0, kFace = 1;

int clip_segment(ClipVertex out[2], const ClipVertex in[2], Vec2 normal, float offset, uint8_t vertex_index_a) {
    int n = 0;
    float d0 = dot(normal, in[0].v) - offset;
    float d1 = dot(normal, in[1].v) - offset;
    if (d0 <= 0) out[n++] = in[0];
    if (d1 <= 0) out[n++] = in[1];
    if (d0 * d1 < 0) {
        float t = d0 / (d0 - d1);
        out[n].v = in[0].v + t * (in[1].v - in[0].v);
        out[n].id = feature(vertex_index_a, uint8_t(in[0].id >> 8), kVertex, kFace);
        n++;
    }
    return n;
}

float max_separation(int* edge, const Shape& p1, const Xf& x1, const Shape& p2, const Xf& x2) {
    // Express p1 in p2's frame.
    Rot q{};
    q.c = x2.q.c * x1.q.c + x2.q.s * x1.q.s;
    q.s = x2.q.c * x1.q.s - x2.q.s * x1.q.c;
    Vec2 t = x2.q.apply_inv(x1.p - x2.p);
    int best = 0;
    float max_sep = -FLT_MAX;
    for (int i = 0; i < p1.count; i++) {
        Vec2 n = q.apply(p1.normals[i]);
        Vec2 v1 = q.apply(p1.vertices[i]) + t;
        float si = FLT_MAX;
        for (int j = 0; j < p2.count; j++) si = std::min(si, dot(n, p2.vertices[j] - v1));
        if (si > max_sep) { max_sep = si; best = i; }
    }
    *edge = best;
    return max_sep;
}

void collide_polygons(Manifold& m, const Shape& A, const Xf& xa, const Shape& B, const Xf& xb) {
    m.count = 0;
    float total_radius = A.radius + B.radius;
    int edge_a, edge_b;
    float sep_a = max_separation(&edge_a, A, xa, B, xb);
    if (sep_a > total_radius) return;
    float sep_b = max_separation(&edge_b, B, xb, A, xa);
    if (sep_b > total_radius) return;

    // Prefer A's face unless B's is clearly better. The tolerance makes the choice stable
    // for parallel edges, so resting boxes do not flip reference faces every step.
    const Shape *p1, *p2;
    Xf x1, x2;
    int edge1;
    uint8_t flip;
    const float tol = 0.1f * kLinearSlop;
    if (sep_b > sep_a + tol) {
        p1 = &B; p2 = &A; x1 = xb; x2 = xa; edge1 = edge_b; m.type = Manifold::FaceB; flip = 1;
    } else {
        p1 = &A; p2 = &B; x1 = xa; x2 = xb; edge1 = edge_a; m.type = Manifold::FaceA; flip = 0;
    }

    // Incident edge: the edge of p2 most anti-parallel to the reference normal.
    Vec2 n1 = x2.q.apply_inv(x1.q.apply(p1->normals[edge1]));
    int inc = 0;
    float min_dot = FLT_MAX;
    for (int i = 0; i < p2->count; i++) {
        float d = dot(n1, p2->normals[i]);
        if (d < min_dot) { min_dot = d; inc = i; }
    }
    int inc2 = (inc + 1) % p2->count;
    ClipVertex incident[2] = {
        {mul(x2, p2->vertices[inc]), feature(uint8_t(edge1), uint8_t(inc), kFace, kVertex)},
        {mul(x2, p2->vertices[inc2]), feature(uint8_t(edge1), uint8_t(inc2), kFace, kVertex)},
    };

    int i11 = edge1, i12 = (edge1 + 1) % p1->count;
    Vec2 v11 = p1->vertices[i11], v12 = p1->vertices[i12];
    Vec2 local_tangent = normalize(v12 - v11);
    Vec2 local_normal = cross(local_tangent, 1.0f);
    Vec2 plane_point = 0.5f * (v11 + v12);
    Vec2 tangent = x1.q.apply(local_tangent);
    Vec2 normal = cross(tangent, 1.0f);
    v11 = mul(x1, v11);
    v12 = mul(x1, v12);
    float front = dot(normal, v11);
    float side1 = -dot(tangent, v11) + total_radius;
    float side2 = dot(tangent, v12) + total_radius;

    ClipVertex c1[2], c2[2];
    if (clip_segment(c1, incident, -tangent, side1, uint8_t(i11)) < 2) return;
    if (clip_segment(c2, c1, tangent, side2, uint8_t(i12)) < 2) return;

    m.local_normal = local_normal;
    m.local_point = plane_point;
    int n = 0;
    for (int i = 0; i < 2; i++) {
        float separation = dot(normal, c2[i].v) - front;
        if (separation <= total_radius) {
            ManifoldPoint& mp = m.points[n];
            mp.local_point = mul_t(x2, c2[i].v);
            uint32_t id = c2[i].id;
            if (flip) {  // swap A/B feature roles so ids stay consistent regardless of reference choice
                uint8_t ia = uint8_t(id), ib = uint8_t(id >> 8), ta = uint8_t(id >> 16), tb = uint8_t(id >> 24);
                id = feature(ib, ia, tb, ta);
            }
            mp.id = id;
            n++;
        }
    }
    m.count = n;
}

void collide_polygon_circle(Manifold& m, const Shape& poly, const Xf& xa, const Shape& circle, const Xf& xb) {
    m.count = 0;
    Vec2 c_local = mul_t(xa, xb.p);
    float radius = poly.radius + circle.radius;
    int normal_index = 0;
    float separation = -FLT_MAX;
    for (int i = 0; i < poly.count; i++) {
        float s = dot(poly.normals[i], c_local - poly.vertices[i]);
        if (s > radius) return;
        if (s > separation) { separation = s; normal_index = i; }
    }
    Vec2 v1 = poly.vertices[normal_index], v2 = poly.vertices[(normal_index + 1) % poly.count];
    m.type = Manifold::FaceA;
    m.points[0].local_point = {0, 0};
    m.points[0].id = 0;
    if (separation < FLT_EPSILON) {  // centre inside the polygon: push out along the nearest face
        m.count = 1;
        m.local_normal = poly.normals[normal_index];
        m.local_point = 0.5f * (v1 + v2);
        return;
    }
    float u1 = dot(c_local - v1, v2 - v1);
    float u2 = dot(c_local - v2, v1 - v2);
    if (u1 <= 0) {         // vertex region v1 (corner contact)
        if (length_sq(c_local - v1) > radius * radius) return;
        m.local_normal = normalize(c_local - v1);
        m.local_point = v1;
    } else if (u2 <= 0) {  // vertex region v2
        if (length_sq(c_local - v2) > radius * radius) return;
        m.local_normal = normalize(c_local - v2);
        m.local_point = v2;
    } else {               // face region (edge contact)
        Vec2 face_center = 0.5f * (v1 + v2);
        if (dot(c_local - face_center, poly.normals[normal_index]) > radius) return;
        m.local_normal = poly.normals[normal_index];
        m.local_point = face_center;
    }
    m.count = 1;
}

void collide_circles(Manifold& m, const Xf& xa, float ra, const Xf& xb, float rb) {
    m.count = 0;
    float r = ra + rb;
    if (length_sq(xb.p - xa.p) > r * r) return;
    m.type = Manifold::Circles;
    m.local_point = {0, 0};
    m.local_normal = {0, 0};
    m.points[0].local_point = {0, 0};
    m.points[0].id = 0;
    m.count = 1;
}
}  // namespace

void collide(const Body& a, const Body& b, Manifold& m) {
    Xf xa = xf_of(a), xb = xf_of(b);
    bool ca = a.shape.type == ShapeType::Circle, cb = b.shape.type == ShapeType::Circle;
    if (ca && cb) collide_circles(m, xa, a.shape.radius, xb, b.shape.radius);
    else if (!ca && cb) collide_polygon_circle(m, a.shape, xa, b.shape, xb);
    else if (!ca && !cb) collide_polygons(m, a.shape, xa, b.shape, xb);
    else m.count = 0;  // circle-vs-polygon is always stored polygon-first by the world
}

// Coincident centres have no defined direction; use a fixed, deterministic one (up).
static Vec2 safe_direction(Vec2 d) {
    return length_sq(d) > FLT_EPSILON * FLT_EPSILON ? normalize(d) : Vec2{0, 1};
}

WorldManifold world_manifold(const Manifold& m, const Body& a, const Body& b) {
    WorldManifold wm{};
    Xf xa = xf_of(a), xb = xf_of(b);
    float ra = a.shape.radius, rb = b.shape.radius;
    if (m.count == 0) return wm;
    switch (m.type) {
        case Manifold::Circles: {
            Vec2 pa = mul(xa, m.local_point), pb = mul(xb, m.points[0].local_point);
            wm.normal = safe_direction(pb - pa);
            Vec2 ca = pa + ra * wm.normal, cb = pb - rb * wm.normal;
            wm.points[0] = 0.5f * (ca + cb);
            wm.separations[0] = dot(cb - ca, wm.normal);
            break;
        }
        case Manifold::FaceA: {
            wm.normal = xa.q.apply(m.local_normal);
            Vec2 plane = mul(xa, m.local_point);
            for (int i = 0; i < m.count; i++) {
                Vec2 clip = mul(xb, m.points[i].local_point);
                Vec2 ca = clip + (ra - dot(clip - plane, wm.normal)) * wm.normal;
                Vec2 cb = clip - rb * wm.normal;
                wm.points[i] = 0.5f * (ca + cb);
                wm.separations[i] = dot(cb - ca, wm.normal);
            }
            break;
        }
        case Manifold::FaceB: {
            wm.normal = xb.q.apply(m.local_normal);
            Vec2 plane = mul(xb, m.local_point);
            for (int i = 0; i < m.count; i++) {
                Vec2 clip = mul(xa, m.points[i].local_point);
                Vec2 cb = clip + (rb - dot(clip - plane, wm.normal)) * wm.normal;
                Vec2 ca = clip - ra * wm.normal;
                wm.points[i] = 0.5f * (ca + cb);
                wm.separations[i] = dot(ca - cb, wm.normal);
            }
            wm.normal = -wm.normal;
            break;
        }
    }
    return wm;
}

// ------------------------------------------------------------------------------ GJK distance

namespace {
struct Proxy {
    Vec2 v[kMaxPolygonVertices];
    int count;
    float radius;
    int support(Vec2 d) const {
        int best = 0;
        float bv = dot(v[0], d);
        for (int i = 1; i < count; i++) { float x = dot(v[i], d); if (x > bv) { bv = x; best = i; } }
        return best;
    }
};
Proxy make_proxy(const Shape& s, Vec2 p, const Rot& r) {
    Proxy px{};
    if (s.type == ShapeType::Circle) { px.count = 1; px.v[0] = p; px.radius = s.radius; return px; }
    px.count = s.count;
    for (int i = 0; i < s.count; i++) px.v[i] = p + r.apply(s.vertices[i]);
    px.radius = s.radius;
    return px;
}
struct SimplexVertex { Vec2 wa, wb, w; float a; int ia, ib; };
struct Simplex {
    SimplexVertex v[3];
    int count;
    void solve2() {
        Vec2 w1 = v[0].w, w2 = v[1].w, e12 = w2 - w1;
        float d12_2 = -dot(w1, e12);
        if (d12_2 <= 0) { v[0].a = 1; count = 1; return; }
        float d12_1 = dot(w2, e12);
        if (d12_1 <= 0) { v[1].a = 1; count = 1; v[0] = v[1]; return; }
        float inv = 1.0f / (d12_1 + d12_2);
        v[0].a = d12_1 * inv; v[1].a = d12_2 * inv; count = 2;
    }
    void solve3() {
        Vec2 w1 = v[0].w, w2 = v[1].w, w3 = v[2].w;
        Vec2 e12 = w2 - w1; float d12_1 = dot(w2, e12), d12_2 = -dot(w1, e12);
        Vec2 e13 = w3 - w1; float d13_1 = dot(w3, e13), d13_2 = -dot(w1, e13);
        Vec2 e23 = w3 - w2; float d23_1 = dot(w3, e23), d23_2 = -dot(w2, e23);
        float n123 = cross(e12, e13);
        float d123_1 = n123 * cross(w2, w3), d123_2 = n123 * cross(w3, w1), d123_3 = n123 * cross(w1, w2);
        if (d12_2 <= 0 && d13_2 <= 0) { v[0].a = 1; count = 1; return; }
        if (d12_1 > 0 && d12_2 > 0 && d123_3 <= 0) { float i = 1 / (d12_1 + d12_2); v[0].a = d12_1 * i; v[1].a = d12_2 * i; count = 2; return; }
        if (d13_1 > 0 && d13_2 > 0 && d123_2 <= 0) { float i = 1 / (d13_1 + d13_2); v[0].a = d13_1 * i; v[2].a = d13_2 * i; count = 2; v[1] = v[2]; return; }
        if (d12_1 <= 0 && d23_2 <= 0) { v[1].a = 1; count = 1; v[0] = v[1]; return; }
        if (d13_1 <= 0 && d23_1 <= 0) { v[2].a = 1; count = 1; v[0] = v[2]; return; }
        if (d23_1 > 0 && d23_2 > 0 && d123_1 <= 0) { float i = 1 / (d23_1 + d23_2); v[1].a = d23_1 * i; v[2].a = d23_2 * i; count = 2; v[0] = v[2]; return; }
        float i = 1 / (d123_1 + d123_2 + d123_3);
        v[0].a = d123_1 * i; v[1].a = d123_2 * i; v[2].a = d123_3 * i; count = 3;
    }
    Vec2 search_direction() const {
        if (count == 1) return -v[0].w;
        Vec2 e12 = v[1].w - v[0].w;
        return cross(e12, -v[0].w) > 0 ? cross(1.0f, e12) : cross(e12, 1.0f);
    }
};
}  // namespace

// Distance between the core shapes (polygon vertices / circle centre), ignoring radii.
static float core_distance(const Proxy& A, const Proxy& B, Vec2& ca, Vec2& cb);

float shape_core_distance(const Shape& sa, Vec2 pa, const Rot& ra, const Shape& sb, Vec2 pb, const Rot& rb) {
    Proxy A = make_proxy(sa, pa, ra), B = make_proxy(sb, pb, rb);
    Vec2 ca, cb;
    return core_distance(A, B, ca, cb);
}

float shape_distance(const Shape& sa, Vec2 pa, const Rot& ra, const Shape& sb, Vec2 pb, const Rot& rb, Vec2* out_a, Vec2* out_b) {
    Proxy A = make_proxy(sa, pa, ra), B = make_proxy(sb, pb, rb);
    Vec2 ca, cb;
    float dist = core_distance(A, B, ca, cb);
    float radii = A.radius + B.radius;
    if (dist > radii && dist > FLT_EPSILON) {
        Vec2 n = (cb - ca) / dist;
        ca += A.radius * n;
        cb -= B.radius * n;
        dist -= radii;
    } else {
        Vec2 mid = 0.5f * (ca + cb);
        ca = cb = mid;
        dist = 0;
    }
    if (out_a) *out_a = ca;
    if (out_b) *out_b = cb;
    return dist;
}

static float core_distance(const Proxy& A, const Proxy& B, Vec2& ca, Vec2& cb) {
    Simplex s{};
    s.count = 1;
    s.v[0] = {A.v[0], B.v[0], B.v[0] - A.v[0], 1, 0, 0};
    int save_a[3], save_b[3];
    for (int iter = 0; iter < 32; iter++) {
        int save_count = s.count;
        for (int i = 0; i < save_count; i++) { save_a[i] = s.v[i].ia; save_b[i] = s.v[i].ib; }
        if (s.count == 2) s.solve2();
        else if (s.count == 3) s.solve3();
        if (s.count == 3) break;  // origin inside: shapes overlap
        Vec2 d = s.search_direction();
        if (length_sq(d) < FLT_EPSILON * FLT_EPSILON) break;
        SimplexVertex& nv = s.v[s.count];
        nv.ia = A.support(-d);
        nv.ib = B.support(d);
        nv.wa = A.v[nv.ia];
        nv.wb = B.v[nv.ib];
        nv.w = nv.wb - nv.wa;
        bool duplicate = false;
        for (int i = 0; i < save_count; i++) if (nv.ia == save_a[i] && nv.ib == save_b[i]) { duplicate = true; break; }
        if (duplicate) break;
        s.count++;
    }
    if (s.count == 1) { ca = s.v[0].wa; cb = s.v[0].wb; }
    else if (s.count == 2) { ca = s.v[0].a * s.v[0].wa + s.v[1].a * s.v[1].wa; cb = s.v[0].a * s.v[0].wb + s.v[1].a * s.v[1].wb; }
    else { ca = s.v[0].a * s.v[0].wa + s.v[1].a * s.v[1].wa + s.v[2].a * s.v[2].wa; cb = ca; }
    return length(cb - ca);
}

}  // namespace unify
