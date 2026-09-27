#pragma once
#include <cstdint>
#include <vector>
#include <string>
#include <map>
#include <mutex>
#include "../core/math.h"

namespace unify {
class BinaryWriter;
class BinaryReader;

enum class InputDevice : uint8_t { Keyboard, Mouse, Gamepad, Touch };
enum class InputEventType : uint8_t { ButtonDown, ButtonUp, ButtonRepeat, Axis, PointerMove };

/// Engine-level key/button codes (platform backends translate into these).
namespace key {
enum : uint16_t {
    Unknown = 0, E = 'e', I = 'i', A = 'a', D = 'd', S = 's', W = 'w', R = 'r', P = 'p',
    Space = 32, Escape = 27, Enter = 13,
    Left = 256, Right, Up, Down, F5, F9, F1, F2, F3, LShift,
    MouseLeft = 512, MouseRight, MouseMiddle,
    PadA = 768, PadB, PadX, PadY, PadStart, PadBack, PadLeft, PadRight, PadUp, PadDown,
    PadAxisLeftX = 1024, PadAxisLeftY,
    Touch0 = 1280,
};
}

struct InputEvent {
    uint64_t timestamp_ns = 0;   // input clock (engine-relative ns); decides which tick consumes it
    InputDevice device = InputDevice::Keyboard;
    InputEventType type = InputEventType::ButtonDown;
    uint16_t code = 0;
    float value = 0;             // axis value, or pointer x
    float value2 = 0;            // pointer y
};

/// Per-tick digested input: what gameplay reads. Serializable, so it can be recorded,
/// replayed, sent over a network, or stored in a save state.
struct InputFrame {
    uint64_t tick = 0;
    std::vector<uint16_t> pressed;    // went down during the tick (even if released in the same tick)
    std::vector<uint16_t> released;   // went up during the tick
    std::vector<uint16_t> repeated;
    std::vector<uint16_t> held;       // down at the end of the tick
    std::map<uint16_t, float> axes;
    Vec2 pointer;
    bool operator==(const InputFrame& o) const {
        return tick == o.tick && pressed == o.pressed && released == o.released && repeated == o.repeated && held == o.held && axes == o.axes && pointer == o.pointer;
    }
    void serialize(BinaryWriter& w) const;
    static InputFrame deserialize(BinaryReader& r);
};

/// OS thread → queue → timestamp → fixed-tick consumption → gameplay.
///
/// Platform code calls push() (thread-safe) as events arrive. At each fixed tick the
/// scheduler calls consume_tick(tick_end_ns), which drains every event stamped before the
/// end of that tick and folds it into an InputFrame. Events stamped later stay queued for
/// the next tick, so a burst of fast frames or a long frame never drops or double-counts.
class InputSystem {
public:
    void push(const InputEvent& e);
    /// Builds the frame for `tick` from all queued events with timestamp < tick_end_ns.
    const InputFrame& consume_tick(uint64_t tick, uint64_t tick_end_ns);
    /// Replay / network path: use a recorded frame instead of live events.
    const InputFrame& apply_frame(const InputFrame& f);

    // Action mapping ("jump" -> Space, W, PadA ...).
    void bind(const std::string& action, uint16_t code);
    void bind_axis(const std::string& action, uint16_t negative, uint16_t positive, uint16_t analog_axis = 0);
    bool is_pressed(const std::string& action) const;
    bool is_held(const std::string& action) const;
    bool is_released(const std::string& action) const;
    bool is_repeated(const std::string& action) const;
    float axis(const std::string& action) const;
    /// True if the action was pressed within the last `ticks` ticks and not yet consumed.
    /// This is the gameplay-side buffer (e.g. jump pressed just before landing).
    bool buffered(const std::string& action, uint32_t ticks) const;
    void consume_buffered(const std::string& action);
    Vec2 pointer() const { return frame_.pointer; }

    const InputFrame& frame() const { return frame_; }
    size_t queued() const { std::lock_guard<std::mutex> l(mutex_); return queue_.size(); }
    uint64_t events_consumed() const { return consumed_; }

    // Recording for deterministic replay.
    void set_recording(bool on) { recording_ = on; if (on) recorded_.clear(); }
    const std::vector<InputFrame>& recorded() const { return recorded_; }

    void serialize(BinaryWriter& w) const;
    void deserialize(BinaryReader& r);

private:
    bool code_in(const std::vector<uint16_t>& list, const std::string& action) const;
    mutable std::mutex mutex_;
    std::vector<InputEvent> queue_;    // guarded by mutex_
    std::vector<InputEvent> drained_;  // tick-local scratch (no per-tick allocation after warmup)
    std::vector<uint16_t> down_;       // keys currently down (sorted)
    std::map<uint16_t, float> axes_;
    Vec2 pointer_;
    InputFrame frame_;
    std::map<std::string, std::vector<uint16_t>> bindings_;
    struct AxisBinding { uint16_t neg, pos, analog; };
    std::map<std::string, AxisBinding> axis_bindings_;
    std::map<std::string, uint64_t> last_pressed_tick_;  // action -> tick+1 of last unconsumed press
    uint64_t consumed_ = 0;
    bool recording_ = false;
    std::vector<InputFrame> recorded_;
};

}  // namespace unify
