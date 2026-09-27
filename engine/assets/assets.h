#pragma once
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "../core/handle.h"
#include "../memory/memory.h"
#include "../render/render.h"
#include "../audio/audio.h"

namespace unify {

enum class AssetType : uint8_t { Texture, Audio, Music, Script, Prefab, Map, Kernel, Material, Font, SaveData };
const char* asset_type_name(AssetType t);

/// Tile map: rows of characters, bottom row last. Legend is interpreted by gameplay.
struct TileMap {
    int width = 0, height = 0;
    float tile_size = 1;
    std::vector<std::string> rows;  // rows[0] = top
    char at(int x, int y_from_bottom) const {
        int r = height - 1 - y_from_bottom;
        if (x < 0 || x >= width || r < 0 || r >= height) return ' ';
        return rows[size_t(r)][size_t(x)];
    }
};

/// Material definition ("shader" asset for the 2D pipeline): blend mode + tint.
struct MaterialDef { BlendMode blend = BlendMode::Alpha; Color tint; };

struct TextureAsset { TextureHandle texture; Atlas atlas; int width = 0, height = 0; };

struct AssetRecord {
    std::string name;      // logical name, e.g. "textures/sprites"
    std::string path;      // file path
    AssetType type;
    uint32_t refs = 0;
    size_t bytes = 0;      // resident bytes attributed to this asset
    // Payloads (only the one matching `type` is set).
    TextureAsset texture;
    std::shared_ptr<AudioClip> clip;
    std::string text;      // script / prefab / kernel source
    TileMap map;
    MaterialDef material;
    uint8_t* blob = nullptr;  // AssetAllocator-owned copy of the file bytes (text assets)
};

/// Registry + cache + loaders. Assets are referenced by AssetHandle and ref-counted:
/// loading an already-resident asset returns the same handle (no duplication), and the
/// payload is freed when the last reference is released.
class AssetManager {
public:
    AssetManager(RenderDevice& device, std::string root, size_t budget_bytes = 256u << 20);
    ~AssetManager();

    AssetHandle load(AssetType type, const std::string& name);
    bool acquire(AssetHandle h);   // add a reference
    bool release(AssetHandle h);   // drop a reference; unloads at zero
    bool valid(AssetHandle h) const { return handles_.valid(h); }
    const AssetRecord* get(AssetHandle h) const { return valid(h) ? &records_[h.index()] : nullptr; }
    AssetHandle find(const std::string& name) const;

    /// Opens a new streaming source for a music asset (streams are per-playback, the file is not cached).
    std::shared_ptr<StreamingSource> open_stream(AssetHandle music);
    std::string path_for(AssetType type, const std::string& name) const;
    const std::string& root() const { return root_; }

    uint32_t live() const { return handles_.alive(); }
    size_t resident_bytes() const;
    const AssetAllocator& allocator() const { return alloc_; }
    std::vector<std::string> loaded_names() const;

private:
    bool load_payload(AssetRecord& r);
    void free_payload(AssetRecord& r);
    RenderDevice& device_;
    std::string root_;
    AssetAllocator alloc_;
    HandlePool<AssetHandle> handles_;
    std::vector<AssetRecord> records_;
    std::map<std::string, AssetHandle> by_name_;
};

bool read_file(const std::string& path, std::vector<uint8_t>& out);
bool write_file(const std::string& path, const std::vector<uint8_t>& data);

}  // namespace unify
