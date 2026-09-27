#pragma once
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <condition_variable>
#include "../core/handle.h"

namespace unify {
class BinaryWriter;
class BinaryReader;

// ------------------------------------------------------------------------------ decoding

struct WavInfo {
    uint16_t channels = 0;
    uint32_t sample_rate = 0;
    uint16_t bits = 0;
    uint32_t frames = 0;
    uint64_t data_offset = 0;
    int64_t loop_start = -1, loop_end = -1;   // from the RIFF 'smpl' chunk, in frames
};

/// Incremental PCM16 WAV decoder over a file. Reads a bounded number of frames at a
/// time; never loads the whole file. Used by streaming sources.
class WavDecoder {
public:
    ~WavDecoder();
    bool open(const std::string& path);
    /// Decodes up to `frames` frames into interleaved stereo float. Returns frames produced.
    uint32_t read(float* stereo_out, uint32_t frames);
    bool seek(uint32_t frame);
    uint32_t position() const { return pos_; }
    const WavInfo& info() const { return info_; }
    bool at_end() const { return pos_ >= info_.frames; }
private:
    FILE* file_ = nullptr;
    WavInfo info_;
    uint32_t pos_ = 0;
    std::vector<int16_t> scratch_;
};

bool parse_wav_header(FILE* f, WavInfo& info);
/// Writes a PCM16 WAV (optionally with a 'smpl' loop) — used by the asset generator.
bool write_wav(const std::string& path, const std::vector<int16_t>& interleaved, uint16_t channels, uint32_t rate, int64_t loop_start = -1, int64_t loop_end = -1);

// ------------------------------------------------------------------------------ assets

/// Fully decoded short sound (SFX). Samples are stereo float at the clip's own rate.
struct AudioClip {
    std::string name;
    uint32_t sample_rate = 44100;
    std::vector<float> samples;   // interleaved stereo
    uint32_t loop_start = 0, loop_end = 0;  // frames; loop_end 0 = whole clip
    uint32_t frames() const { return uint32_t(samples.size() / 2); }
    bool load_wav(const std::string& path);
};

/// Single-producer/single-consumer ring of stereo frames between the streamer thread
/// (decoder side) and the mixer (device side). Lock-free.
class StreamBuffer {
public:
    explicit StreamBuffer(uint32_t capacity_frames) : buf_(size_t(capacity_frames) * 2), cap_(capacity_frames) {}
    uint32_t available() const { return uint32_t(write_.load(std::memory_order_acquire) - read_.load(std::memory_order_acquire)); }
    uint32_t space() const { return cap_ - available(); }
    uint32_t capacity() const { return cap_; }
    uint32_t write(const float* frames, uint32_t n);
    uint32_t read(float* frames, uint32_t n);
    void reset() { read_.store(0); write_.store(0); }
private:
    std::vector<float> buf_;
    uint32_t cap_;
    std::atomic<uint64_t> read_{0}, write_{0};
};

/// Long audio played from disk: decoder → stream buffer. Owns its decoder.
struct StreamingSource {
    std::string name, path;
    WavDecoder decoder;
    StreamBuffer buffer{44100};           // ~1 s at 44.1 kHz
    uint32_t loop_start = 0, loop_end = 0;
    bool loop = false;
    std::atomic<bool> finished{false};    // decoder reached the end (non-looping)
    std::atomic<uint64_t> underruns{0};
    std::atomic<uint64_t> frames_consumed{0};  // frames the mixer has pulled (for save/seek)
    std::mutex decoder_mutex;
    bool open(const std::string& p);
    /// Decodes into the buffer until it is at least half full (called by the streamer thread).
    void refill();
    void seek(uint32_t frame);
};

// ------------------------------------------------------------------------------ mixer

enum class Bus : uint8_t { Master = 0, Sfx = 1, Music = 2, Count = 3 };
enum class VoiceState : uint8_t { Free, Playing, Paused };

struct PlayParams {
    Bus bus = Bus::Sfx;
    float volume = 1, pan = 0, pitch = 1;
    bool loop = false;
    float fade_in = 0;   // seconds
    double start = 0;    // clip start position in frames (save-state restore)
};

/// Voice mixer. Game thread issues commands; the audio device thread calls render().
/// Voice slots are preallocated: starting a sound never allocates on the audio thread.
class Mixer {
public:
    explicit Mixer(uint32_t device_rate = 44100, uint32_t max_voices = 64);

    AudioHandle play(std::shared_ptr<AudioClip> clip, const PlayParams& p = {});
    AudioHandle stream(std::shared_ptr<StreamingSource> src, const PlayParams& p = {});
    bool stop(AudioHandle h, float fade_out = 0);
    bool pause(AudioHandle h);
    bool resume(AudioHandle h);
    bool set_volume(AudioHandle h, float v);
    bool set_pan(AudioHandle h, float pan);
    bool set_pitch(AudioHandle h, float pitch);
    bool fade(AudioHandle h, float target, float seconds);
    bool playing(AudioHandle h) const;
    /// Voice still exists (playing or paused).
    bool alive(AudioHandle h) const;
    void set_bus_volume(Bus b, float v);
    float bus_volume(Bus b) const;
    void stop_all();

    /// Mixes `frames` stereo frames into `out` (interleaved float, [-1,1]). Audio thread.
    void render(float* out, uint32_t frames);

    uint32_t device_rate() const { return rate_; }
    /// Audio clock: frames rendered by the device since start.
    uint64_t frames_rendered() const { return rendered_.load(); }
    double audio_time() const { return double(rendered_.load()) / rate_; }
    uint32_t active_voices() const;
    float peak() const { return peak_.load(); }

    /// Voices that reference streams, for the streamer thread.
    void streams(std::vector<std::shared_ptr<StreamingSource>>& out) const;

    // Save-state support: voices are persisted by asset name + playback position.
    struct VoiceSnapshot { AudioHandle handle; std::string asset; bool is_stream; Bus bus; float volume, pan, pitch; bool loop; bool paused; double position; };
    std::vector<VoiceSnapshot> snapshot() const;

private:
    struct Voice {
        VoiceState state = VoiceState::Free;
        uint8_t generation = 1;
        std::shared_ptr<AudioClip> clip;
        std::shared_ptr<StreamingSource> stream;
        Bus bus = Bus::Sfx;
        float volume = 1, pan = 0, pitch = 1;
        bool loop = false;
        double position = 0;         // clip frame position (fractional)
        float gain = 1, gain_target = 1, gain_step = 0;  // fade envelope
        bool stop_after_fade = false;
        float prev[2] = {0, 0}, next[2] = {0, 0};  // stream interpolation window
        double frac = 1;             // stream: fraction between prev and next
    };
    Voice* find(AudioHandle h);
    const Voice* find(AudioHandle h) const;
    AudioHandle start(Voice*& v, const PlayParams& p);

    uint32_t rate_;
    std::vector<Voice> voices_;
    float bus_volume_[int(Bus::Count)] = {1, 1, 1};
    mutable std::mutex mutex_;
    std::atomic<uint64_t> rendered_{0};
    std::atomic<float> peak_{0};
};

// ------------------------------------------------------------------------------ devices

/// Output device abstraction. Backends provide real devices; the engine ships a null
/// device (real-time thread, no output) and an offline device (manual pull, for tests).
class AudioDevice {
public:
    virtual ~AudioDevice() = default;
    virtual bool start(Mixer& mixer) = 0;
    virtual void stop() = 0;
    virtual const char* name() const = 0;
};

/// Renders the mix on its own thread at real-time pace and discards it. Keeps audio time
/// advancing independently of rendering when no hardware device is present.
class NullAudioDevice : public AudioDevice {
public:
    ~NullAudioDevice() override { stop(); }
    bool start(Mixer& mixer) override;
    void stop() override;
    const char* name() const override { return "null"; }
private:
    std::thread thread_;
    std::atomic<bool> running_{false};
};

/// Background decoder: keeps every active stream's buffer topped up.
class AudioStreamer {
public:
    ~AudioStreamer() { stop(); }
    void start(Mixer& mixer);
    void stop();
    /// Synchronous refill (deterministic tests, or single-threaded platforms).
    static void pump(Mixer& mixer);
private:
    std::thread thread_;
    std::atomic<bool> running_{false};
};

}  // namespace unify
