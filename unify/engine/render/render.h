#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "../core/math.h"
#include "../core/handle.h"

namespace unify {

struct RectI { int x = 0, y = 0, w = 0, h = 0; bool empty() const { return w <= 0 || h <= 0; } };
struct RectF { float x = 0, y = 0, w = 0, h = 0; };

/// CPU-side RGBA8 image. Also the storage of the software render target.
struct Image {
    int width = 0, height = 0;
    std::vector<uint8_t> pixels;  // RGBA, row-major, top row first
    Image() = default;
    Image(int w, int h, Color fill = {0, 0, 0, 0}) : width(w), height(h), pixels(size_t(w) * h * 4) {
        for (size_t i = 0; i < pixels.size(); i += 4) { pixels[i] = fill.r; pixels[i + 1] = fill.g; pixels[i + 2] = fill.b; pixels[i + 3] = fill.a; }
    }
    uint8_t* at(int x, int y) { return &pixels[(size_t(y) * width + x) * 4]; }
    const uint8_t* at(int x, int y) const { return &pixels[(size_t(y) * width + x) * 4]; }
};

bool decode_qoi(const std::vector<uint8_t>& data, Image& out);
std::vector<uint8_t> encode_qoi(const Image& img);
bool write_png(const std::string& path, const Image& img);   // uncompressed deflate, for screenshots
bool load_image(const std::string& path, Image& out);         // .qoi

enum class BlendMode : uint8_t { Opaque, Alpha, Additive, Multiply };

struct Material {
    BlendMode blend = BlendMode::Alpha;
    bool operator==(const Material& o) const { return blend == o.blend; }
};

/// Texture atlas: named sub-rectangles of one texture, packed with a shelf packer.
struct AtlasRegion { std::string name; RectI rect; };
struct Atlas {
    std::vector<AtlasRegion> regions;  // region 0 is the whole texture
    int find(const std::string& name) const {
        for (size_t i = 0; i < regions.size(); i++) if (regions[i].name == name) return int(i);
        return -1;
    }
};
/// Packs images into one atlas image; returns false if they do not fit.
bool pack_atlas(const std::vector<std::pair<std::string, const Image*>>& items, int size, Image& out, Atlas& atlas, int padding = 1);

struct Camera {
    Vec2 position;             // world point at the viewport centre
    float zoom = 32;           // pixels per world unit
    float rotation = 0;
};

struct Viewport { RectI rect; };

/// A render target: a colour buffer the device draws into.
struct RenderTarget {
    Image color;
    RenderTarget() = default;
    RenderTarget(int w, int h) : color(w, h, {0, 0, 0, 255}) {}
    int width() const { return color.width; }
    int height() const { return color.height; }
};

/// One queued draw. Everything is resolved to target pixels by the Renderer2D before
/// reaching the device, so devices only rasterize textured quads.
struct RenderCommand {
    uint32_t texture = 0;       // TextureHandle bits (0 = white)
    RectF src;                  // texel rectangle
    Vec2 corners[4];            // screen-space, CCW from bottom-left of the sprite
    Color tint;
    int16_t layer = 0;
    float depth = 0;            // within a layer: larger draws later (in front)
    Material material;
    RectI scissor;              // empty = full viewport
    uint32_t sequence = 0;      // submission order: final tie-breaker, keeps sort stable
};

struct Vertex { float x, y, u, v; Color color; };

/// A run of commands sharing texture, material and scissor: one device draw call.
struct Batch {
    uint32_t texture = 0;
    Material material;
    RectI scissor;
    const Vertex* vertices = nullptr;   // 4 per quad
    uint32_t quad_count = 0;
};

/// Abstract GPU-or-CPU device. Backends implement this (software, SDL, Unreal).
class RenderDevice {
public:
    virtual ~RenderDevice() = default;
    virtual TextureHandle create_texture(const Image& image) = 0;
    virtual bool update_texture(TextureHandle h, const Image& image) = 0;
    virtual bool destroy_texture(TextureHandle h) = 0;
    virtual bool texture_size(TextureHandle h, int& w, int& h_out) const = 0;
    virtual void begin(RenderTarget& target, Color clear) = 0;
    virtual void draw(const Batch& batch) = 0;
    virtual void end() = 0;
    virtual uint32_t live_textures() const = 0;
};

/// Reference device: rasterizes batches into the target's colour buffer on the CPU.
class SoftwareRenderDevice : public RenderDevice {
public:
    TextureHandle create_texture(const Image& image) override;
    bool update_texture(TextureHandle h, const Image& image) override;
    bool destroy_texture(TextureHandle h) override;
    bool texture_size(TextureHandle h, int& w, int& h_out) const override;
    void begin(RenderTarget& target, Color clear) override;
    void draw(const Batch& batch) override;
    void end() override {}
    uint32_t live_textures() const override { return handles_.alive(); }
private:
    void raster_triangle(const Vertex& a, const Vertex& b, const Vertex& c, const Image* tex, BlendMode blend, const RectI& clip);
    HandlePool<TextureHandle> handles_;
    std::vector<Image> textures_;
    RenderTarget* target_ = nullptr;
    Image white_{1, 1, {255, 255, 255, 255}};
};

struct RenderStats { uint32_t commands = 0, batches = 0, quads = 0; };

/// Collects commands for a frame, sorts and batches them, and submits to a device.
class Renderer2D {
public:
    explicit Renderer2D(RenderDevice& device) : device_(device) { commands_.reserve(4096); vertices_.reserve(4096 * 4); }

    void begin(RenderTarget& target, const Camera& camera, const Viewport& vp, Color clear = {20, 22, 30, 255});
    /// World-space sprite: centre position, size in world units, rotation.
    void sprite(uint32_t texture, RectF src, Vec2 pos, Vec2 size, float rotation, Color tint, int16_t layer, float depth,
                BlendMode blend = BlendMode::Alpha, bool flip_x = false, Vec2 pivot = {0.5f, 0.5f});
    /// Screen-space rectangle (UI): pixels, origin top-left.
    void screen_rect(uint32_t texture, RectF src, RectF dst, Color tint, int16_t layer, BlendMode blend = BlendMode::Alpha);
    void fill_screen_rect(RectF dst, Color c, int16_t layer) { screen_rect(0, {0, 0, 1, 1}, dst, c, layer); }
    void set_scissor(RectI r) { scissor_ = r; }
    void clear_scissor() { scissor_ = {}; }
    void end();

    Vec2 world_to_screen(Vec2 w) const;
    Vec2 screen_to_world(Vec2 s) const;
    const RenderStats& stats() const { return stats_; }
    /// Stats of the last completed frame (valid while a new frame is being built).
    const RenderStats& last_frame_stats() const { return last_stats_; }
    RenderDevice& device() { return device_; }

private:
    RenderDevice& device_;
    RenderTarget* target_ = nullptr;
    Camera camera_;
    Viewport viewport_;
    Color clear_;
    RectI scissor_;
    std::vector<RenderCommand> commands_;
    std::vector<Vertex> vertices_;
    uint32_t sequence_ = 0;
    RenderStats stats_, last_stats_;
};

/// Bitmap font (5x7 glyphs baked into an atlas-ready image, 6x8 cells).
struct BitmapFont {
    Image image;                   // 16x6 grid of 6x8 cells for ASCII 32..127
    uint32_t texture = 0;
    static BitmapFont build();
    RectF glyph(char c) const;
    /// Draws text at a screen position (top-left), scale = pixels per font pixel.
    void draw(Renderer2D& r, const std::string& text, Vec2 pos, float scale, Color color, int16_t layer) const;
    float width(const std::string& text, float scale) const { return float(text.size()) * 6 * scale; }
};

}  // namespace unify
