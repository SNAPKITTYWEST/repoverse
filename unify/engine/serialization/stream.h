#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <stdexcept>
#include "../core/math.h"

namespace unify {

/// Little-endian binary writer. Floats are written as their IEEE-754 bit patterns,
/// so a round trip is bit-exact (required for deterministic save states).
class BinaryWriter {
public:
    void u8(uint8_t v) { buf_.push_back(v); }
    void u16(uint16_t v) { for (int i = 0; i < 2; i++) buf_.push_back(uint8_t(v >> (8 * i))); }
    void u32(uint32_t v) { for (int i = 0; i < 4; i++) buf_.push_back(uint8_t(v >> (8 * i))); }
    void u64(uint64_t v) { for (int i = 0; i < 8; i++) buf_.push_back(uint8_t(v >> (8 * i))); }
    void i32(int32_t v) { u32(uint32_t(v)); }
    void f32(float v) { uint32_t b; std::memcpy(&b, &v, 4); u32(b); }
    void f64(double v) { uint64_t b; std::memcpy(&b, &v, 8); u64(b); }
    void boolean(bool v) { u8(v ? 1 : 0); }
    void vec2(Vec2 v) { f32(v.x); f32(v.y); }
    void str(const std::string& s) { u32(uint32_t(s.size())); bytes(s.data(), s.size()); }
    void bytes(const void* p, size_t n) { auto b = static_cast<const uint8_t*>(p); buf_.insert(buf_.end(), b, b + n); }
    void blob(const std::vector<uint8_t>& v) { u32(uint32_t(v.size())); bytes(v.data(), v.size()); }

    /// Tagged, length-prefixed section. Readers can skip sections they do not understand,
    /// which is what lets the save format evolve without breaking old files.
    size_t begin_section(const char tag[4]) { bytes(tag, 4); size_t at = buf_.size(); u32(0); return at; }
    void end_section(size_t at) {
        uint32_t len = uint32_t(buf_.size() - at - 4);
        for (int i = 0; i < 4; i++) buf_[at + i] = uint8_t(len >> (8 * i));
    }

    const std::vector<uint8_t>& data() const { return buf_; }
    std::vector<uint8_t> take() { return std::move(buf_); }
    size_t size() const { return buf_.size(); }
private:
    std::vector<uint8_t> buf_;
};

struct SerializationError : std::runtime_error { using std::runtime_error::runtime_error; };

class BinaryReader {
public:
    BinaryReader(const uint8_t* data, size_t size) : p_(data), end_(data + size) {}
    explicit BinaryReader(const std::vector<uint8_t>& v) : BinaryReader(v.data(), v.size()) {}
    uint8_t u8() { need(1); return *p_++; }
    uint16_t u16() { need(2); uint16_t v = uint16_t(p_[0] | (p_[1] << 8)); p_ += 2; return v; }
    uint32_t u32() { need(4); uint32_t v = 0; for (int i = 0; i < 4; i++) v |= uint32_t(p_[i]) << (8 * i); p_ += 4; return v; }
    uint64_t u64() { need(8); uint64_t v = 0; for (int i = 0; i < 8; i++) v |= uint64_t(p_[i]) << (8 * i); p_ += 8; return v; }
    int32_t i32() { return int32_t(u32()); }
    float f32() { uint32_t b = u32(); float v; std::memcpy(&v, &b, 4); return v; }
    double f64() { uint64_t b = u64(); double v; std::memcpy(&v, &b, 8); return v; }
    bool boolean() { return u8() != 0; }
    Vec2 vec2() { float x = f32(); return {x, f32()}; }
    std::string str() { uint32_t n = u32(); need(n); std::string s(reinterpret_cast<const char*>(p_), n); p_ += n; return s; }
    void bytes(void* out, size_t n) { need(n); std::memcpy(out, p_, n); p_ += n; }
    std::vector<uint8_t> blob() { uint32_t n = u32(); need(n); std::vector<uint8_t> v(p_, p_ + n); p_ += n; return v; }

    /// Reads the next section header. Returns a reader limited to the section body.
    bool next_section(char tag[5], BinaryReader& body) {
        if (p_ == end_) return false;
        need(8);
        std::memcpy(tag, p_, 4); tag[4] = 0; p_ += 4;
        uint32_t len = u32();
        need(len);
        body = BinaryReader(p_, len);
        p_ += len;
        return true;
    }
    bool at_end() const { return p_ == end_; }
    size_t remaining() const { return size_t(end_ - p_); }
private:
    void need(size_t n) const { if (size_t(end_ - p_) < n) throw SerializationError("unexpected end of data"); }
    const uint8_t* p_;
    const uint8_t* end_;
};

}  // namespace unify
