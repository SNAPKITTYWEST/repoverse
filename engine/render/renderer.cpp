#include "render.h"
#include <algorithm>
#include <cmath>

namespace unify {

// ------------------------------------------------------------------------------ software device

TextureHandle SoftwareRenderDevice::create_texture(const Image& image) {
    TextureHandle h = handles_.allocate();
    if (h.index() >= textures_.size()) textures_.resize(h.index() + 1);
    textures_[h.index()] = image;
    return h;
}
bool SoftwareRenderDevice::update_texture(TextureHandle h, const Image& image) {
    if (!handles_.valid(h)) return false;
    textures_[h.index()] = image;
    return true;
}
bool SoftwareRenderDevice::destroy_texture(TextureHandle h) {
    if (!handles_.valid(h)) return false;
    textures_[h.index()] = Image{};
    return handles_.release(h);
}
bool SoftwareRenderDevice::texture_size(TextureHandle h, int& w, int& hh) const {
    if (!handles_.valid(h)) return false;
    w = textures_[h.index()].width;
    hh = textures_[h.index()].height;
    return true;
}

void SoftwareRenderDevice::begin(RenderTarget& target, Color clear) {
    target_ = &target;
    auto& px = target.color.pixels;
    for (size_t i = 0; i < px.size(); i += 4) { px[i] = clear.r; px[i + 1] = clear.g; px[i + 2] = clear.b; px[i + 3] = clear.a; }
}

static inline uint8_t mul8(int a, int b) { return uint8_t((a * b + 127) / 255); }

void SoftwareRenderDevice::raster_triangle(const Vertex& a, const Vertex& b, const Vertex& c, const Image* tex, BlendMode blend, const RectI& clip) {
    Image& dst = target_->color;
    float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    if (std::fabs(area) < 1e-6f) return;
    int minx = std::max(clip.x, int(std::floor(std::min({a.x, b.x, c.x}))));
    int maxx = std::min(clip.x + clip.w - 1, int(std::ceil(std::max({a.x, b.x, c.x}))));
    int miny = std::max(clip.y, int(std::floor(std::min({a.y, b.y, c.y}))));
    int maxy = std::min(clip.y + clip.h - 1, int(std::ceil(std::max({a.y, b.y, c.y}))));
    if (minx > maxx || miny > maxy) return;
    float inv = 1.0f / area;
    const int tw = tex->width, th = tex->height;
    for (int y = miny; y <= maxy; y++) {
        float py = y + 0.5f;
        for (int x = minx; x <= maxx; x++) {
            float px = x + 0.5f;
            // Barycentric weights; the top-left rule is approximated by >= 0 on shared edges.
            float w0 = ((b.x - px) * (c.y - py) - (b.y - py) * (c.x - px)) * inv;
            float w1 = ((c.x - px) * (a.y - py) - (c.y - py) * (a.x - px)) * inv;
            float w2 = 1.0f - w0 - w1;
            if (w0 < 0 || w1 < 0 || w2 < 0) continue;
            float u = w0 * a.u + w1 * b.u + w2 * c.u;
            float v = w0 * a.v + w1 * b.v + w2 * c.v;
            int tx = std::clamp(int(std::floor(u)), 0, tw - 1);
            int ty = std::clamp(int(std::floor(v)), 0, th - 1);
            const uint8_t* s = tex->at(tx, ty);
            int sr = mul8(s[0], a.color.r), sg = mul8(s[1], a.color.g), sb = mul8(s[2], a.color.b), sa = mul8(s[3], a.color.a);
            uint8_t* d = dst.at(x, y);
            switch (blend) {
                case BlendMode::Opaque: d[0] = uint8_t(sr); d[1] = uint8_t(sg); d[2] = uint8_t(sb); d[3] = 255; break;
                case BlendMode::Alpha:
                    if (sa == 0) break;
                    d[0] = uint8_t((sr * sa + d[0] * (255 - sa) + 127) / 255);
                    d[1] = uint8_t((sg * sa + d[1] * (255 - sa) + 127) / 255);
                    d[2] = uint8_t((sb * sa + d[2] * (255 - sa) + 127) / 255);
                    d[3] = uint8_t(std::min(255, sa + d[3] * (255 - sa) / 255));
                    break;
                case BlendMode::Additive:
                    d[0] = uint8_t(std::min(255, d[0] + sr * sa / 255));
                    d[1] = uint8_t(std::min(255, d[1] + sg * sa / 255));
                    d[2] = uint8_t(std::min(255, d[2] + sb * sa / 255));
                    break;
                case BlendMode::Multiply:
                    d[0] = mul8(d[0], sr); d[1] = mul8(d[1], sg); d[2] = mul8(d[2], sb);
                    break;
            }
        }
    }
}

void SoftwareRenderDevice::draw(const Batch& batch) {
    if (!target_) return;
    const Image* tex = &white_;
    if (batch.texture) {
        TextureHandle h{batch.texture};
        if (handles_.valid(h) && textures_[h.index()].width > 0) tex = &textures_[h.index()];
    }
    RectI clip = batch.scissor.empty() ? RectI{0, 0, target_->width(), target_->height()} : batch.scissor;
    clip.x = std::max(clip.x, 0);
    clip.y = std::max(clip.y, 0);
    clip.w = std::min(clip.w, target_->width() - clip.x);
    clip.h = std::min(clip.h, target_->height() - clip.y);
    if (clip.empty()) return;
    for (uint32_t q = 0; q < batch.quad_count; q++) {
        const Vertex* v = batch.vertices + q * 4;
        raster_triangle(v[0], v[1], v[2], tex, batch.material.blend, clip);
        raster_triangle(v[0], v[2], v[3], tex, batch.material.blend, clip);
    }
}

// ------------------------------------------------------------------------------ Renderer2D

void Renderer2D::begin(RenderTarget& target, const Camera& camera, const Viewport& vp, Color clear) {
    target_ = &target;
    camera_ = camera;
    viewport_ = vp.rect.empty() ? Viewport{{0, 0, target.width(), target.height()}} : vp;
    clear_ = clear;
    commands_.clear();
    sequence_ = 0;
    scissor_ = {};
    stats_ = {};
}

Vec2 Renderer2D::world_to_screen(Vec2 w) const {
    Vec2 d = Rot(-camera_.rotation).apply(w - camera_.position) * camera_.zoom;
    return {viewport_.rect.x + viewport_.rect.w * 0.5f + d.x, viewport_.rect.y + viewport_.rect.h * 0.5f - d.y};  // y up -> y down
}
Vec2 Renderer2D::screen_to_world(Vec2 s) const {
    Vec2 d{s.x - (viewport_.rect.x + viewport_.rect.w * 0.5f), -(s.y - (viewport_.rect.y + viewport_.rect.h * 0.5f))};
    return camera_.position + Rot(camera_.rotation).apply(d / camera_.zoom);
}

void Renderer2D::sprite(uint32_t texture, RectF src, Vec2 pos, Vec2 size, float rotation, Color tint, int16_t layer, float depth,
                        BlendMode blend, bool flip_x, Vec2 pivot) {
    RenderCommand c;
    c.texture = texture;
    c.src = src;
    if (flip_x) { c.src.x += c.src.w; c.src.w = -c.src.w; }
    Rot r(rotation);
    Vec2 local[4] = {{-pivot.x * size.x, -pivot.y * size.y}, {(1 - pivot.x) * size.x, -pivot.y * size.y},
                     {(1 - pivot.x) * size.x, (1 - pivot.y) * size.y}, {-pivot.x * size.x, (1 - pivot.y) * size.y}};
    AABB bounds{{1e30f, 1e30f}, {-1e30f, -1e30f}};
    for (int i = 0; i < 4; i++) {
        c.corners[i] = world_to_screen(pos + r.apply(local[i]));
        bounds.min = vmin(bounds.min, c.corners[i]);
        bounds.max = vmax(bounds.max, c.corners[i]);
    }
    // Cull off-screen sprites before they cost a sort slot.
    const RectI& vr = viewport_.rect;
    if (bounds.max.x < vr.x || bounds.min.x > vr.x + vr.w || bounds.max.y < vr.y || bounds.min.y > vr.y + vr.h) return;
    c.tint = tint; c.layer = layer; c.depth = depth; c.material.blend = blend;
    c.scissor = scissor_.empty() ? vr : scissor_;
    c.sequence = sequence_++;
    commands_.push_back(c);
}

void Renderer2D::screen_rect(uint32_t texture, RectF src, RectF dst, Color tint, int16_t layer, BlendMode blend) {
    RenderCommand c;
    c.texture = texture;
    c.src = src;
    c.corners[0] = {dst.x, dst.y + dst.h};
    c.corners[1] = {dst.x + dst.w, dst.y + dst.h};
    c.corners[2] = {dst.x + dst.w, dst.y};
    c.corners[3] = {dst.x, dst.y};
    c.tint = tint; c.layer = layer; c.depth = 0; c.material.blend = blend;
    c.scissor = scissor_;
    c.sequence = sequence_++;
    commands_.push_back(c);
}

void Renderer2D::end() {
    if (!target_) return;
    // Sort: layer, then depth, then keep batchable state together, then submission order.
    // Depth decides visibility, so state-grouping only happens among equal depths.
    std::sort(commands_.begin(), commands_.end(), [](const RenderCommand& a, const RenderCommand& b) {
        if (a.layer != b.layer) return a.layer < b.layer;
        if (a.depth != b.depth) return a.depth < b.depth;
        if (a.material.blend != b.material.blend) return a.material.blend < b.material.blend;
        if (a.texture != b.texture) return a.texture < b.texture;
        return a.sequence < b.sequence;
    });
    vertices_.clear();
    for (const RenderCommand& c : commands_) {
        // Quad vertex order: bottom-left, bottom-right, top-right, top-left (source v is top-down).
        float u0 = c.src.x, u1 = c.src.x + c.src.w, v0 = c.src.y + c.src.h, v1 = c.src.y;
        vertices_.push_back({c.corners[0].x, c.corners[0].y, u0, v0, c.tint});
        vertices_.push_back({c.corners[1].x, c.corners[1].y, u1, v0, c.tint});
        vertices_.push_back({c.corners[2].x, c.corners[2].y, u1, v1, c.tint});
        vertices_.push_back({c.corners[3].x, c.corners[3].y, u0, v1, c.tint});
    }
    device_.begin(*target_, clear_);
    size_t i = 0;
    while (i < commands_.size()) {
        size_t j = i + 1;
        auto same = [&](const RenderCommand& a, const RenderCommand& b) {
            return a.texture == b.texture && a.material == b.material && a.scissor.x == b.scissor.x && a.scissor.y == b.scissor.y &&
                   a.scissor.w == b.scissor.w && a.scissor.h == b.scissor.h;
        };
        while (j < commands_.size() && same(commands_[i], commands_[j])) j++;
        Batch b;
        b.texture = commands_[i].texture;
        b.material = commands_[i].material;
        b.scissor = commands_[i].scissor;
        b.vertices = &vertices_[i * 4];
        b.quad_count = uint32_t(j - i);
        device_.draw(b);
        stats_.batches++;
        i = j;
    }
    device_.end();
    stats_.commands = stats_.quads = uint32_t(commands_.size());
    last_stats_ = stats_;
}

// ------------------------------------------------------------------------------ atlas

bool pack_atlas(const std::vector<std::pair<std::string, const Image*>>& items, int size, Image& out, Atlas& atlas, int padding) {
    out = Image(size, size);
    atlas.regions.clear();
    atlas.regions.push_back({"", {0, 0, size, size}});
    // Shelf packing, tallest first (stable for equal heights so output is deterministic).
    std::vector<size_t> order(items.size());
    for (size_t i = 0; i < order.size(); i++) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return items[a].second->height > items[b].second->height; });
    std::vector<RectI> placed(items.size());
    int x = padding, y = padding, shelf = 0;
    for (size_t k : order) {
        const Image& im = *items[k].second;
        if (x + im.width + padding > size) { x = padding; y += shelf + padding; shelf = 0; }
        if (y + im.height + padding > size) return false;
        placed[k] = {x, y, im.width, im.height};
        for (int row = 0; row < im.height; row++)
            std::copy(im.at(0, row), im.at(0, row) + im.width * 4, out.at(x, y + row));
        x += im.width + padding;
        shelf = std::max(shelf, im.height);
    }
    for (size_t i = 0; i < items.size(); i++) atlas.regions.push_back({items[i].first, placed[i]});
    return true;
}

}  // namespace unify
