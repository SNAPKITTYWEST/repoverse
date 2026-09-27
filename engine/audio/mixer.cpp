#include "audio.h"
#include "../core/core.h"
#include "../core/math.h"
#include <cmath>
#include <chrono>
#include <algorithm>

namespace unify {

// ------------------------------------------------------------------------------ stream buffer

uint32_t StreamBuffer::write(const float* frames, uint32_t n) {
    uint64_t w = write_.load(std::memory_order_relaxed), r = read_.load(std::memory_order_acquire);
    n = std::min<uint32_t>(n, cap_ - uint32_t(w - r));
    for (uint32_t i = 0; i < n; i++) {
        size_t at = size_t((w + i) % cap_) * 2;
        buf_[at] = frames[2 * i];
        buf_[at + 1] = frames[2 * i + 1];
    }
    write_.store(w + n, std::memory_order_release);
    return n;
}

uint32_t StreamBuffer::read(float* frames, uint32_t n) {
    uint64_t r = read_.load(std::memory_order_relaxed), w = write_.load(std::memory_order_acquire);
    n = std::min<uint32_t>(n, uint32_t(w - r));
    for (uint32_t i = 0; i < n; i++) {
        size_t at = size_t((r + i) % cap_) * 2;
        frames[2 * i] = buf_[at];
        frames[2 * i + 1] = buf_[at + 1];
    }
    read_.store(r + n, std::memory_order_release);
    return n;
}

bool StreamingSource::open(const std::string& p) {
    path = p;
    if (!decoder.open(p)) return false;
    const WavInfo& i = decoder.info();
    loop_start = i.loop_start >= 0 ? uint32_t(i.loop_start) : 0;
    loop_end = i.loop_end > 0 ? uint32_t(i.loop_end) : i.frames;
    return true;
}

void StreamingSource::refill() {
    std::lock_guard<std::mutex> lock(decoder_mutex);
    float chunk[2 * 2048];
    while (buffer.space() > buffer.capacity() / 2) {
        uint32_t end = loop ? loop_end : decoder.info().frames;
        uint32_t pos = decoder.position();
        if (pos >= end) {
            if (!loop) { finished = true; return; }
            decoder.seek(loop_start);  // loop point: continue seamlessly from loop_start
            continue;
        }
        uint32_t want = std::min<uint32_t>({2048, end - pos, buffer.space()});
        uint32_t got = decoder.read(chunk, want);
        if (got == 0) { finished = true; return; }
        buffer.write(chunk, got);
    }
}

void StreamingSource::seek(uint32_t frame) {
    std::lock_guard<std::mutex> lock(decoder_mutex);
    decoder.seek(frame);
    buffer.reset();
    finished = false;
}

// ------------------------------------------------------------------------------ mixer

Mixer::Mixer(uint32_t device_rate, uint32_t max_voices) : rate_(device_rate), voices_(max_voices) {}

Mixer::Voice* Mixer::find(AudioHandle h) {
    if (h.is_null() || h.index() >= voices_.size()) return nullptr;
    Voice& v = voices_[h.index()];
    return v.state != VoiceState::Free && v.generation == h.generation() ? &v : nullptr;
}
const Mixer::Voice* Mixer::find(AudioHandle h) const { return const_cast<Mixer*>(this)->find(h); }

AudioHandle Mixer::start(Voice*& out, const PlayParams& p) {
    out = nullptr;
    for (uint32_t i = 0; i < voices_.size(); i++) {
        Voice& v = voices_[i];
        if (v.state != VoiceState::Free) continue;
        uint8_t gen = v.generation;
        v = Voice{};
        v.generation = gen;
        v.state = VoiceState::Playing;
        v.bus = p.bus; v.volume = p.volume; v.pan = clampf(p.pan, -1, 1); v.pitch = std::max(0.01f, p.pitch); v.loop = p.loop;
        if (p.fade_in > 0) { v.gain = 0; v.gain_target = 1; v.gain_step = 1.0f / (p.fade_in * rate_); }
        out = &v;
        return AudioHandle::make(i, gen);
    }
    UNIFY_LOG_WARN("AUDIO", "voice limit (%zu) reached; sound dropped", voices_.size());
    return {};
}

AudioHandle Mixer::play(std::shared_ptr<AudioClip> clip, const PlayParams& p) {
    if (!clip || clip->frames() == 0) return {};
    std::lock_guard<std::mutex> lock(mutex_);
    Voice* v;
    AudioHandle h = start(v, p);
    if (v) { v->clip = std::move(clip); v->position = std::max(0.0, p.start); }
    return h;
}

AudioHandle Mixer::stream(std::shared_ptr<StreamingSource> src, const PlayParams& p) {
    if (!src) return {};
    src->loop = p.loop;
    src->refill();  // prime the buffer so playback starts without an underrun
    std::lock_guard<std::mutex> lock(mutex_);
    Voice* v;
    AudioHandle h = start(v, p);
    if (v) { v->stream = std::move(src); v->frac = 1; }
    return h;
}


bool Mixer::stop(AudioHandle h, float fade_out) {
    std::lock_guard<std::mutex> lock(mutex_);
    Voice* v = find(h);
    if (!v) return false;
    if (fade_out > 0) {
        v->gain_target = 0;
        v->gain_step = v->gain / (fade_out * rate_);
        v->stop_after_fade = true;
        return true;
    }
    v->state = VoiceState::Free;
    v->generation = uint8_t(v->generation + 1) ? uint8_t(v->generation + 1) : 1;
    v->clip.reset();
    v->stream.reset();
    return true;
}
bool Mixer::pause(AudioHandle h) { std::lock_guard<std::mutex> l(mutex_); Voice* v = find(h); if (!v) return false; v->state = VoiceState::Paused; return true; }
bool Mixer::resume(AudioHandle h) { std::lock_guard<std::mutex> l(mutex_); Voice* v = find(h); if (!v) return false; v->state = VoiceState::Playing; return true; }
bool Mixer::set_volume(AudioHandle h, float x) { std::lock_guard<std::mutex> l(mutex_); Voice* v = find(h); if (!v) return false; v->volume = std::max(0.0f, x); return true; }
bool Mixer::set_pan(AudioHandle h, float x) { std::lock_guard<std::mutex> l(mutex_); Voice* v = find(h); if (!v) return false; v->pan = clampf(x, -1, 1); return true; }
bool Mixer::set_pitch(AudioHandle h, float x) { std::lock_guard<std::mutex> l(mutex_); Voice* v = find(h); if (!v) return false; v->pitch = std::max(0.01f, x); return true; }
bool Mixer::fade(AudioHandle h, float target, float seconds) {
    std::lock_guard<std::mutex> l(mutex_);
    Voice* v = find(h);
    if (!v) return false;
    v->gain_target = std::max(0.0f, target);
    v->gain_step = seconds > 0 ? std::fabs(v->gain_target - v->gain) / (seconds * rate_) : 1e9f;
    return true;
}
bool Mixer::alive(AudioHandle h) const { std::lock_guard<std::mutex> l(mutex_); return find(h) != nullptr; }
bool Mixer::playing(AudioHandle h) const { std::lock_guard<std::mutex> l(mutex_); const Voice* v = find(h); return v && v->state == VoiceState::Playing; }
void Mixer::set_bus_volume(Bus b, float v) { std::lock_guard<std::mutex> l(mutex_); bus_volume_[int(b)] = std::max(0.0f, v); }
float Mixer::bus_volume(Bus b) const { std::lock_guard<std::mutex> l(mutex_); return bus_volume_[int(b)]; }
void Mixer::stop_all() {
    std::lock_guard<std::mutex> l(mutex_);
    for (auto& v : voices_) if (v.state != VoiceState::Free) { v.state = VoiceState::Free; v.generation = uint8_t(v.generation + 1) ? uint8_t(v.generation + 1) : 1; v.clip.reset(); v.stream.reset(); }
}
uint32_t Mixer::active_voices() const {
    std::lock_guard<std::mutex> l(mutex_);
    uint32_t n = 0;
    for (auto& v : voices_) n += v.state != VoiceState::Free;
    return n;
}
void Mixer::streams(std::vector<std::shared_ptr<StreamingSource>>& out) const {
    out.clear();
    std::lock_guard<std::mutex> l(mutex_);
    for (auto& v : voices_) if (v.state != VoiceState::Free && v.stream) out.push_back(v.stream);
}

void Mixer::render(float* out, uint32_t frames) {
    std::fill(out, out + size_t(frames) * 2, 0.0f);
    std::lock_guard<std::mutex> lock(mutex_);
    const float master = bus_volume_[int(Bus::Master)];
    for (Voice& v : voices_) {
        if (v.state != VoiceState::Playing) continue;
        const float bus = bus_volume_[int(v.bus)] * master;
        // Constant-power pan.
        const float angle = (v.pan + 1) * 0.25f * 3.14159265f;
        const float pl = std::cos(angle) * 1.41421356f, pr = std::sin(angle) * 1.41421356f;
        bool done = false;
        for (uint32_t i = 0; i < frames && !done; i++) {
            float l, r;
            if (v.clip) {
                const AudioClip& c = *v.clip;
                uint32_t end = (v.loop && c.loop_end) ? c.loop_end : c.frames();
                uint32_t i0 = uint32_t(v.position);
                if (i0 >= end) {
                    if (v.loop) { v.position = c.loop_start + (v.position - end); i0 = uint32_t(v.position); }
                    else { done = true; break; }
                }
                uint32_t i1 = i0 + 1 < end ? i0 + 1 : (v.loop ? c.loop_start : i0);
                float t = float(v.position - i0);
                l = lerp(c.samples[2 * i0], c.samples[2 * i1], t);
                r = lerp(c.samples[2 * i0 + 1], c.samples[2 * i1 + 1], t);
                v.position += double(v.pitch) * c.sample_rate / rate_;
            } else {
                StreamingSource& s = *v.stream;
                double stepv = double(v.pitch) * s.decoder.info().sample_rate / rate_;
                while (v.frac >= 1) {
                    float fr[2];
                    if (s.buffer.read(fr, 1) == 0) {
                        if (s.finished) { done = true; break; }
                        s.underruns++;
                        fr[0] = v.next[0]; fr[1] = v.next[1];  // hold last sample; streamer will catch up
                    } else {
                        s.frames_consumed++;
                    }
                    v.prev[0] = v.next[0]; v.prev[1] = v.next[1];
                    v.next[0] = fr[0]; v.next[1] = fr[1];
                    v.frac -= 1;
                }
                if (done) break;
                l = lerp(v.prev[0], v.next[0], float(v.frac));
                r = lerp(v.prev[1], v.next[1], float(v.frac));
                v.frac += stepv;
            }
            if (v.gain != v.gain_target) {
                v.gain = v.gain < v.gain_target ? std::min(v.gain + v.gain_step, v.gain_target) : std::max(v.gain - v.gain_step, v.gain_target);
                if (v.gain == 0 && v.stop_after_fade) { done = true; }
            }
            float g = v.volume * v.gain * bus;
            out[2 * i] += l * g * pl;
            out[2 * i + 1] += r * g * pr;
        }
        if (done) {
            v.state = VoiceState::Free;
            v.generation = uint8_t(v.generation + 1) ? uint8_t(v.generation + 1) : 1;
            v.clip.reset();
            v.stream.reset();
        }
    }
    float pk = 0;
    for (uint32_t i = 0; i < frames * 2; i++) {
        out[i] = clampf(out[i], -1, 1);  // hard limiter: never emit out-of-range samples
        pk = std::max(pk, std::fabs(out[i]));
    }
    peak_.store(pk);
    rendered_ += frames;
}

std::vector<Mixer::VoiceSnapshot> Mixer::snapshot() const {
    std::lock_guard<std::mutex> l(mutex_);
    std::vector<VoiceSnapshot> out;
    for (uint32_t i = 0; i < voices_.size(); i++) {
        const Voice& v = voices_[i];
        if (v.state == VoiceState::Free) continue;
        VoiceSnapshot s;
        s.handle = AudioHandle::make(i, v.generation);
        s.is_stream = bool(v.stream);
        s.asset = v.clip ? v.clip->name : v.stream->name;
        s.bus = v.bus; s.volume = v.volume; s.pan = v.pan; s.pitch = v.pitch; s.loop = v.loop;
        s.paused = v.state == VoiceState::Paused;
        s.position = v.clip ? v.position : double(v.stream->decoder.position());
        if (v.stream) {
            // Decoder position runs ahead of playback by what is buffered; report what was heard.
            uint64_t buffered = v.stream->buffer.available();
            double pos = s.position - double(buffered);
            if (pos < 0) pos += v.stream->loop_end - v.stream->loop_start;
            s.position = pos;
        }
        out.push_back(s);
    }
    return out;
}

// ------------------------------------------------------------------------------ devices

bool NullAudioDevice::start(Mixer& mixer) {
    if (running_) return true;
    running_ = true;
    thread_ = std::thread([this, &mixer] {
        const uint32_t block = 512;
        std::vector<float> buf(block * 2);
        auto next = std::chrono::steady_clock::now();
        const auto period = std::chrono::nanoseconds(uint64_t(block) * 1000000000ull / mixer.device_rate());
        while (running_) {
            mixer.render(buf.data(), block);
            next += period;
            std::this_thread::sleep_until(next);
        }
    });
    return true;
}
void NullAudioDevice::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

void AudioStreamer::pump(Mixer& mixer) {
    std::vector<std::shared_ptr<StreamingSource>> list;
    mixer.streams(list);
    for (auto& s : list) s->refill();
}
void AudioStreamer::start(Mixer& mixer) {
    if (running_) return;
    running_ = true;
    thread_ = std::thread([this, &mixer] {
        std::vector<std::shared_ptr<StreamingSource>> list;
        while (running_) {
            mixer.streams(list);
            for (auto& s : list) s->refill();
            list.clear();
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    });
}
void AudioStreamer::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

}  // namespace unify
