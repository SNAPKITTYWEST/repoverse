#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace unify {
class BinaryWriter;
class BinaryReader;

enum class PlayMode : uint8_t { Once, Loop, PingPong };

struct Keyframe { float time; float value; };

/// Linear-interpolated float track (e.g. "scale_y", "rotation", "alpha").
struct Track {
    std::string property;
    std::vector<Keyframe> keys;   // sorted by time
    float sample(float t) const;
};

/// Sprite frame: atlas region index shown for `duration` seconds.
struct SpriteFrame { uint32_t region; float duration; };

struct AnimationClip {
    std::string name;
    PlayMode mode = PlayMode::Loop;
    std::vector<SpriteFrame> frames;   // sprite animation (may be empty)
    std::vector<Track> tracks;         // timeline properties (may be empty)
    float length() const;              // max of frame total and last key time
};

/// Plays one clip at a time with optional crossfade from the previous clip.
/// Time is advanced only by the simulation (fixed dt), never by render frames.
struct AnimationPlayer {
    std::string clip;
    float time = 0;
    float speed = 1;
    bool finished = false;
    // Crossfade state: the previous clip keeps advancing while its weight falls to 0.
    std::string from_clip;
    float from_time = 0;
    float blend = 1;          // weight of `clip` (1 = no blend in progress)
    float blend_duration = 0;
};

class AnimationSystem {
public:
    void add_clip(AnimationClip clip);
    const AnimationClip* clip(const std::string& name) const;

    uint32_t create_player(const std::string& clip);
    bool destroy_player(uint32_t id);
    AnimationPlayer* player(uint32_t id);
    /// Switches clip. With blend_seconds > 0 track values crossfade from the current clip.
    void play(uint32_t id, const std::string& clip, float blend_seconds = 0, bool restart = false);
    void advance(float dt);

    /// Current sprite region for the player (from the active clip's frame at its time).
    uint32_t sprite_region(uint32_t id) const;
    /// Blended track value; returns `fallback` if neither clip animates the property.
    float value(uint32_t id, const std::string& property, float fallback) const;

    size_t live_players() const;
    void serialize(BinaryWriter& w) const;
    void deserialize(BinaryReader& r);
    void clear_players() { players_.clear(); free_.clear(); }

private:
    static float wrap(const AnimationClip& c, float t, bool* finished);
    std::map<std::string, AnimationClip> clips_;
    std::vector<AnimationPlayer> players_;
    std::vector<uint8_t> live_;
    std::vector<uint32_t> free_;
};

}  // namespace unify
