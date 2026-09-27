#include "assets.h"
#include "../core/core.h"
#include <cstdio>
#include <cstring>
#include <sstream>

namespace unify {

const char* asset_type_name(AssetType t) {
    switch (t) {
        case AssetType::Texture: return "texture";
        case AssetType::Audio: return "audio";
        case AssetType::Music: return "music";
        case AssetType::Script: return "script";
        case AssetType::Prefab: return "prefab";
        case AssetType::Map: return "map";
        case AssetType::Kernel: return "kernel";
        case AssetType::Material: return "material";
        case AssetType::Font: return "font";
        case AssetType::SaveData: return "save";
    }
    return "?";
}

bool read_file(const std::string& path, std::vector<uint8_t>& out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    out.clear();
    uint8_t buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.insert(out.end(), buf, buf + n);
    std::fclose(f);
    return true;
}

bool write_file(const std::string& path, const std::vector<uint8_t>& data) {
    std::string tmp = path + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return false;
    bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
    ok = std::fclose(f) == 0 && ok;
    return ok && std::rename(tmp.c_str(), path.c_str()) == 0;  // atomic replace: no torn save files
}

AssetManager::AssetManager(RenderDevice& device, std::string root, size_t budget) : device_(device), root_(std::move(root)), alloc_(budget) {}

AssetManager::~AssetManager() {
    for (uint32_t i = 0; i < handles_.capacity(); i++)
        if (AssetHandle h = handles_.at(i)) free_payload(records_[i]);
}

std::string AssetManager::path_for(AssetType type, const std::string& name) const {
    static const std::map<AssetType, const char*> ext = {
        {AssetType::Texture, ".qoi"}, {AssetType::Audio, ".wav"}, {AssetType::Music, ".wav"}, {AssetType::Script, ".lua"},
        {AssetType::Prefab, ".lua"}, {AssetType::Map, ".map"}, {AssetType::Kernel, ".cu"}, {AssetType::Material, ".mat"},
        {AssetType::Font, ""}, {AssetType::SaveData, ".usav"}};
    return root_ + "/" + name + ext.at(type);
}

AssetHandle AssetManager::find(const std::string& name) const {
    auto it = by_name_.find(name);
    return it == by_name_.end() ? AssetHandle{} : it->second;
}

AssetHandle AssetManager::load(AssetType type, const std::string& name) {
    auto it = by_name_.find(name);
    if (it != by_name_.end() && handles_.valid(it->second)) {
        AssetRecord& r = records_[it->second.index()];
        if (r.type != type) { UNIFY_LOG_ERROR("ASSETS", "'%s' already loaded as %s", name.c_str(), asset_type_name(r.type)); return {}; }
        r.refs++;
        return it->second;  // cached: no duplicate load
    }
    AssetRecord rec;
    rec.name = name;
    rec.type = type;
    rec.path = path_for(type, name);
    rec.refs = 1;
    if (!load_payload(rec)) {
        UNIFY_LOG_ERROR("ASSETS", "failed to load %s '%s' (%s)", asset_type_name(type), name.c_str(), rec.path.c_str());
        return {};
    }
    AssetHandle h = handles_.allocate();
    if (h.index() >= records_.size()) records_.resize(h.index() + 1);
    records_[h.index()] = std::move(rec);
    by_name_[name] = h;
    return h;
}

bool AssetManager::acquire(AssetHandle h) {
    if (!valid(h)) return false;
    records_[h.index()].refs++;
    return true;
}

bool AssetManager::release(AssetHandle h) {
    if (!valid(h)) return false;
    AssetRecord& r = records_[h.index()];
    if (--r.refs > 0) return true;
    free_payload(r);
    by_name_.erase(r.name);
    r = AssetRecord{};
    handles_.release(h);
    return true;
}

static bool parse_map(const std::string& text, TileMap& m) {
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("//", 0) == 0) continue;  // comment ('#' is a tile character)
        if (line.rfind("tile_size ", 0) == 0) { m.tile_size = std::stof(line.substr(10)); continue; }
        m.rows.push_back(line);
        m.width = std::max(m.width, int(line.size()));
    }
    for (auto& r : m.rows) r.resize(size_t(m.width), ' ');
    m.height = int(m.rows.size());
    return m.height > 0;
}

bool AssetManager::load_payload(AssetRecord& r) {
    switch (r.type) {
        case AssetType::Texture: {
            Image img;
            if (!load_image(r.path, img)) return false;
            r.texture.texture = device_.create_texture(img);
            r.texture.width = img.width;
            r.texture.height = img.height;
            r.texture.atlas.regions.push_back({"", {0, 0, img.width, img.height}});
            // Optional atlas sidecar: "name x y w h" per line.
            std::vector<uint8_t> meta;
            std::string atlas_path = r.path.substr(0, r.path.size() - 4) + ".atlas";
            if (read_file(atlas_path, meta)) {
                std::istringstream in(std::string(meta.begin(), meta.end()));
                AtlasRegion reg;
                while (in >> reg.name >> reg.rect.x >> reg.rect.y >> reg.rect.w >> reg.rect.h) r.texture.atlas.regions.push_back(reg);
            }
            r.bytes = img.pixels.size();
            return bool(r.texture.texture);
        }
        case AssetType::Audio: {
            r.clip = std::make_shared<AudioClip>();
            r.clip->name = r.name;
            if (!r.clip->load_wav(r.path)) return false;
            r.bytes = r.clip->samples.size() * sizeof(float);
            return true;
        }
        case AssetType::Music: {
            // Streamed: validate the header only. Nothing but the header is read here.
            WavDecoder probe;
            if (!probe.open(r.path)) return false;
            r.bytes = 0;
            return true;
        }
        case AssetType::Font: return true;
        case AssetType::Script: case AssetType::Prefab: case AssetType::Map: case AssetType::Kernel: case AssetType::Material: case AssetType::SaveData: {
            std::vector<uint8_t> data;
            if (!read_file(r.path, data)) return false;
            r.blob = alloc_.allocate(data.size() + 1, asset_type_name(r.type));
            if (!r.blob) { UNIFY_LOG_ERROR("ASSETS", "asset budget exhausted loading '%s'", r.name.c_str()); return false; }
            std::memcpy(r.blob, data.data(), data.size());
            r.blob[data.size()] = 0;
            r.text.assign(reinterpret_cast<char*>(r.blob), data.size());
            r.bytes = data.size() + 1;
            if (r.type == AssetType::Map && !parse_map(r.text, r.map)) return false;
            if (r.type == AssetType::Material) {
                std::istringstream in(r.text);
                std::string key;
                while (in >> key) {
                    if (key == "blend") {
                        std::string v; in >> v;
                        r.material.blend = v == "additive" ? BlendMode::Additive : v == "multiply" ? BlendMode::Multiply : v == "opaque" ? BlendMode::Opaque : BlendMode::Alpha;
                    } else if (key == "tint") {
                        int cr, cg, cb, ca; in >> cr >> cg >> cb >> ca;
                        r.material.tint = {uint8_t(cr), uint8_t(cg), uint8_t(cb), uint8_t(ca)};
                    }
                }
            }
            return true;
        }
    }
    return false;
}

void AssetManager::free_payload(AssetRecord& r) {
    if (r.type == AssetType::Texture && r.texture.texture) device_.destroy_texture(r.texture.texture);
    if (r.blob) { alloc_.free(r.blob); r.blob = nullptr; }
    r.clip.reset();
}

std::shared_ptr<StreamingSource> AssetManager::open_stream(AssetHandle h) {
    const AssetRecord* r = get(h);
    if (!r || r->type != AssetType::Music) return nullptr;
    auto s = std::make_shared<StreamingSource>();
    s->name = r->name;
    if (!s->open(r->path)) return nullptr;
    return s;
}

size_t AssetManager::resident_bytes() const {
    size_t n = 0;
    for (uint32_t i = 0; i < handles_.capacity(); i++) if (handles_.at(i)) n += records_[i].bytes;
    return n;
}

std::vector<std::string> AssetManager::loaded_names() const {
    std::vector<std::string> out;
    for (auto& kv : by_name_) out.push_back(kv.first);
    return out;
}

}  // namespace unify
