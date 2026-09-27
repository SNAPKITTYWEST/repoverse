#pragma once
#include <cstdint>
#include <deque>
#include <string>
#include <vector>
#include "../input/input.h"

namespace unify {
class Engine;

/// Networking-ready state model: periodic snapshots + the exact per-tick input log.
///
/// Because a UNIFY simulation is a pure function of (state, input frame), a peer that learns
/// late that the input for some past tick was different can rewind to the nearest snapshot at
/// or before that tick, substitute the corrected frame, and re-simulate to the present. This is
/// the core of rollback netcode; lockstep peers instead compare state_hash() per tick.
class RollbackBuffer {
public:
    explicit RollbackBuffer(uint32_t snapshot_interval = 10, uint32_t max_history_ticks = 600)
        : interval_(snapshot_interval), max_ticks_(max_history_ticks) {}

    /// Runs one live tick through the engine and records it (snapshotting every interval).
    void advance(Engine& engine);
    /// Replaces the input for `tick` and re-simulates up to the current tick.
    bool correct(Engine& engine, uint64_t tick, const InputFrame& frame, std::string& error);
    /// Re-simulates from the snapshot at or before `tick` using the recorded frames.
    bool rewind_and_resimulate(Engine& engine, uint64_t tick, std::string& error);

    size_t snapshots() const { return snaps_.size(); }
    const std::deque<InputFrame>& log() const { return log_; }
    uint64_t resimulated_ticks() const { return resimulated_; }

private:
    struct Snap { uint64_t tick; std::vector<uint8_t> state; };
    uint32_t interval_, max_ticks_;
    std::deque<Snap> snaps_;
    std::deque<InputFrame> log_;   // log_[i].tick is consecutive
    uint64_t resimulated_ = 0;
};

}  // namespace unify
