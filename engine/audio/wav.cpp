#include "audio.h"
#include <cstring>

namespace unify {

static uint32_t rd32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
static uint16_t rd16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }

bool parse_wav_header(FILE* f, WavInfo& info) {
    uint8_t hdr[12];
    if (std::fread(hdr, 1, 12, f) != 12 || std::memcmp(hdr, "RIFF", 4) || std::memcmp(hdr + 8, "WAVE", 4)) return false;
    bool have_fmt = false, have_data = false;
    uint8_t ch[8];
    while (std::fread(ch, 1, 8, f) == 8) {
        uint32_t size = rd32(ch + 4);
        long body = std::ftell(f);
        if (!std::memcmp(ch, "fmt ", 4)) {
            uint8_t fmt[16];
            if (size < 16 || std::fread(fmt, 1, 16, f) != 16) return false;
            if (rd16(fmt) != 1) return false;  // PCM only
            info.channels = rd16(fmt + 2);
            info.sample_rate = rd32(fmt + 4);
            info.bits = rd16(fmt + 14);
            have_fmt = true;
        } else if (!std::memcmp(ch, "data", 4)) {
            info.data_offset = uint64_t(body);
            if (have_fmt && info.channels) info.frames = size / (uint32_t(info.channels) * (info.bits / 8));
            have_data = true;
        } else if (!std::memcmp(ch, "smpl", 4) && size >= 36 + 24) {
            std::vector<uint8_t> s(size);
            if (std::fread(s.data(), 1, size, f) != size) return false;
            if (rd32(&s[28]) > 0) {  // num sample loops
                info.loop_start = rd32(&s[36 + 8]);
                info.loop_end = int64_t(rd32(&s[36 + 12])) + 1;  // 'end' is inclusive in the spec
            }
        }
        std::fseek(f, body + long(size + (size & 1)), SEEK_SET);
    }
    return have_fmt && have_data && info.bits == 16 && (info.channels == 1 || info.channels == 2);
}

WavDecoder::~WavDecoder() { if (file_) std::fclose(file_); }

bool WavDecoder::open(const std::string& path) {
    if (file_) std::fclose(file_);
    file_ = std::fopen(path.c_str(), "rb");
    if (!file_ || !parse_wav_header(file_, info_)) { if (file_) std::fclose(file_); file_ = nullptr; return false; }
    return seek(0);
}

bool WavDecoder::seek(uint32_t frame) {
    if (!file_) return false;
    if (frame > info_.frames) frame = info_.frames;
    pos_ = frame;
    return std::fseek(file_, long(info_.data_offset + uint64_t(frame) * info_.channels * 2), SEEK_SET) == 0;
}

uint32_t WavDecoder::read(float* out, uint32_t frames) {
    if (!file_) return 0;
    uint32_t n = std::min(frames, info_.frames - pos_);
    scratch_.resize(size_t(n) * info_.channels);
    size_t got = std::fread(scratch_.data(), 2 * info_.channels, n, file_);
    for (size_t i = 0; i < got; i++) {
        if (info_.channels == 2) {
            out[2 * i] = scratch_[2 * i] / 32768.0f;
            out[2 * i + 1] = scratch_[2 * i + 1] / 32768.0f;
        } else {
            out[2 * i] = out[2 * i + 1] = scratch_[i] / 32768.0f;
        }
    }
    pos_ += uint32_t(got);
    return uint32_t(got);
}

bool write_wav(const std::string& path, const std::vector<int16_t>& pcm, uint16_t channels, uint32_t rate, int64_t loop_start, int64_t loop_end) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    auto w32 = [&](uint32_t v) { uint8_t b[4] = {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)}; std::fwrite(b, 1, 4, f); };
    auto w16 = [&](uint16_t v) { uint8_t b[2] = {uint8_t(v), uint8_t(v >> 8)}; std::fwrite(b, 1, 2, f); };
    uint32_t data_bytes = uint32_t(pcm.size() * 2);
    bool has_loop = loop_start >= 0 && loop_end > loop_start;
    uint32_t smpl_size = has_loop ? 36 + 24 : 0;
    std::fwrite("RIFF", 1, 4, f);
    w32(4 + (8 + 16) + (8 + data_bytes) + (has_loop ? 8 + smpl_size : 0));
    std::fwrite("WAVE", 1, 4, f);
    std::fwrite("fmt ", 1, 4, f); w32(16);
    w16(1); w16(channels); w32(rate); w32(rate * channels * 2); w16(uint16_t(channels * 2)); w16(16);
    if (has_loop) {
        std::fwrite("smpl", 1, 4, f); w32(smpl_size);
        for (int i = 0; i < 7; i++) w32(i == 2 ? 1000000000u / rate : 0);  // manufacturer..SMPTE; [2]=sample period ns
        w32(1); w32(0);                                                    // num loops, sampler data
        w32(0); w32(0); w32(uint32_t(loop_start)); w32(uint32_t(loop_end - 1)); w32(0); w32(0);
    }
    std::fwrite("data", 1, 4, f); w32(data_bytes);
    std::fwrite(pcm.data(), 2, pcm.size(), f);
    return std::fclose(f) == 0;
}

bool AudioClip::load_wav(const std::string& path) {
    WavDecoder d;
    if (!d.open(path)) return false;
    sample_rate = d.info().sample_rate;
    samples.resize(size_t(d.info().frames) * 2);
    uint32_t got = d.read(samples.data(), d.info().frames);
    samples.resize(size_t(got) * 2);
    loop_start = d.info().loop_start >= 0 ? uint32_t(d.info().loop_start) : 0;
    loop_end = d.info().loop_end > 0 ? uint32_t(d.info().loop_end) : 0;
    return true;
}

}  // namespace unify
