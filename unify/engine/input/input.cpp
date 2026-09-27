#include "input.h"
#include "../serialization/stream.h"
#include <algorithm>

namespace unify {

static void insert_sorted(std::vector<uint16_t>& v, uint16_t c) {
    auto it = std::lower_bound(v.begin(), v.end(), c);
    if (it == v.end() || *it != c) v.insert(it, c);
}
static void erase_sorted(std::vector<uint16_t>& v, uint16_t c) {
    auto it = std::lower_bound(v.begin(), v.end(), c);
    if (it != v.end() && *it == c) v.erase(it);
}

void InputSystem::push(const InputEvent& e) {
    std::lock_guard<std::mutex> lock(mutex_);
    // Keep the queue ordered by timestamp even if devices report slightly out of order.
    auto it = std::upper_bound(queue_.begin(), queue_.end(), e.timestamp_ns,
                               [](uint64_t t, const InputEvent& x) { return t < x.timestamp_ns; });
    queue_.insert(it, e);
}

const InputFrame& InputSystem::consume_tick(uint64_t tick, uint64_t tick_end_ns) {
    drained_.clear();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        size_t n = 0;
        while (n < queue_.size() && queue_[n].timestamp_ns < tick_end_ns) n++;
        drained_.insert(drained_.end(), queue_.begin(), queue_.begin() + long(n));
        queue_.erase(queue_.begin(), queue_.begin() + long(n));
    }
    InputFrame& f = frame_;
    f.tick = tick;
    f.pressed.clear(); f.released.clear(); f.repeated.clear();
    for (const InputEvent& e : drained_) {
        switch (e.type) {
            case InputEventType::ButtonDown:
                // A down while already down (lost up event) still counts as held, not a new press.
                if (!std::binary_search(down_.begin(), down_.end(), e.code)) insert_sorted(f.pressed, e.code);
                insert_sorted(down_, e.code);
                break;
            case InputEventType::ButtonUp:
                if (std::binary_search(down_.begin(), down_.end(), e.code)) insert_sorted(f.released, e.code);
                erase_sorted(down_, e.code);
                break;
            case InputEventType::ButtonRepeat: insert_sorted(f.repeated, e.code); break;
            case InputEventType::Axis: axes_[e.code] = clampf(e.value, -1, 1); break;
            case InputEventType::PointerMove: pointer_ = {e.value, e.value2}; break;
        }
    }
    consumed_ += drained_.size();
    f.held = down_;
    f.axes = axes_;
    f.pointer = pointer_;
    for (auto& kv : bindings_)
        if (code_in(f.pressed, kv.first)) last_pressed_tick_[kv.first] = tick + 1;
    if (recording_) recorded_.push_back(f);
    return f;
}

const InputFrame& InputSystem::apply_frame(const InputFrame& in) {
    frame_ = in;
    down_ = in.held;
    axes_ = in.axes;
    pointer_ = in.pointer;
    for (auto& kv : bindings_)
        if (code_in(frame_.pressed, kv.first)) last_pressed_tick_[kv.first] = in.tick + 1;
    if (recording_) recorded_.push_back(frame_);
    return frame_;
}

void InputSystem::bind(const std::string& action, uint16_t code) { bindings_[action].push_back(code); }
void InputSystem::bind_axis(const std::string& action, uint16_t neg, uint16_t pos, uint16_t analog) { axis_bindings_[action] = {neg, pos, analog}; }

bool InputSystem::code_in(const std::vector<uint16_t>& list, const std::string& action) const {
    auto it = bindings_.find(action);
    if (it == bindings_.end()) return false;
    for (uint16_t c : it->second) if (std::binary_search(list.begin(), list.end(), c)) return true;
    return false;
}
bool InputSystem::is_pressed(const std::string& a) const { return code_in(frame_.pressed, a); }
bool InputSystem::is_held(const std::string& a) const { return code_in(frame_.held, a) || code_in(frame_.pressed, a); }
bool InputSystem::is_released(const std::string& a) const { return code_in(frame_.released, a); }
bool InputSystem::is_repeated(const std::string& a) const { return code_in(frame_.repeated, a); }

float InputSystem::axis(const std::string& action) const {
    auto it = axis_bindings_.find(action);
    if (it == axis_bindings_.end()) return 0;
    float v = 0;
    if (std::binary_search(frame_.held.begin(), frame_.held.end(), it->second.neg)) v -= 1;
    if (std::binary_search(frame_.held.begin(), frame_.held.end(), it->second.pos)) v += 1;
    if (it->second.analog) {
        auto a = frame_.axes.find(it->second.analog);
        if (a != frame_.axes.end() && std::fabs(a->second) > 0.2f) v += a->second;  // dead zone
    }
    return clampf(v, -1, 1);
}

bool InputSystem::buffered(const std::string& action, uint32_t ticks) const {
    auto it = last_pressed_tick_.find(action);
    if (it == last_pressed_tick_.end() || it->second == 0) return false;
    uint64_t pressed_tick = it->second - 1;
    return frame_.tick - pressed_tick <= ticks;
}
void InputSystem::consume_buffered(const std::string& action) { last_pressed_tick_[action] = 0; }

// ------------------------------------------------------------------------------ serialization

static void write_codes(BinaryWriter& w, const std::vector<uint16_t>& v) { w.u16(uint16_t(v.size())); for (auto c : v) w.u16(c); }
static std::vector<uint16_t> read_codes(BinaryReader& r) { std::vector<uint16_t> v(r.u16()); for (auto& c : v) c = r.u16(); return v; }

void InputFrame::serialize(BinaryWriter& w) const {
    w.u64(tick);
    write_codes(w, pressed); write_codes(w, released); write_codes(w, repeated); write_codes(w, held);
    w.u16(uint16_t(axes.size()));
    for (auto& kv : axes) { w.u16(kv.first); w.f32(kv.second); }
    w.vec2(pointer);
}
InputFrame InputFrame::deserialize(BinaryReader& r) {
    InputFrame f;
    f.tick = r.u64();
    f.pressed = read_codes(r); f.released = read_codes(r); f.repeated = read_codes(r); f.held = read_codes(r);
    uint16_t n = r.u16();
    for (uint16_t i = 0; i < n; i++) { uint16_t c = r.u16(); f.axes[c] = r.f32(); }
    f.pointer = r.vec2();
    return f;
}

void InputSystem::serialize(BinaryWriter& w) const {
    frame_.serialize(w);
    write_codes(w, down_);
    w.u16(uint16_t(last_pressed_tick_.size()));
    for (auto& kv : last_pressed_tick_) { w.str(kv.first); w.u64(kv.second); }
}
void InputSystem::deserialize(BinaryReader& r) {
    frame_ = InputFrame::deserialize(r);
    down_ = read_codes(r);
    axes_ = frame_.axes;
    pointer_ = frame_.pointer;
    last_pressed_tick_.clear();
    uint16_t n = r.u16();
    for (uint16_t i = 0; i < n; i++) { std::string a = r.str(); last_pressed_tick_[a] = r.u64(); }
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.clear();  // live events from before the restore belong to a timeline that no longer exists
}

}  // namespace unify
