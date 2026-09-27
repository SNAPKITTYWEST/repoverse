#include "rollback.h"
#include "../runtime/engine.h"

namespace unify {

void RollbackBuffer::advance(Engine& engine) {
    uint64_t tick = engine.sim_clock().tick;
    if (tick % interval_ == 0) snaps_.push_back({tick, engine.save_state(false)});
    engine.tick();
    log_.push_back(engine.input().frame());
    // Bound history: drop snapshots (and the input they cover) older than the window.
    while (!snaps_.empty() && tick - snaps_.front().tick > max_ticks_ && snaps_.size() > 1) snaps_.pop_front();
    while (!log_.empty() && !snaps_.empty() && log_.front().tick < snaps_.front().tick) log_.pop_front();
}

bool RollbackBuffer::rewind_and_resimulate(Engine& engine, uint64_t tick, std::string& error) {
    uint64_t now = engine.sim_clock().tick;
    const Snap* base = nullptr;
    for (const Snap& s : snaps_) if (s.tick <= tick) base = &s;
    if (!base) { error = "tick is older than the rollback window"; return false; }
    if (!engine.load_state(base->state, error)) return false;
    for (const InputFrame& f : log_) {
        if (f.tick < base->tick) continue;
        if (f.tick >= now) break;
        if (f.tick % interval_ == 0) {  // refresh snapshots along the corrected timeline
            for (Snap& s : snaps_) if (s.tick == f.tick) s.state = engine.save_state(false);
        }
        engine.replay_tick(f);
        resimulated_++;
    }
    return engine.sim_clock().tick == now || (error = "resimulation ended at the wrong tick", false);
}

bool RollbackBuffer::correct(Engine& engine, uint64_t tick, const InputFrame& frame, std::string& error) {
    for (InputFrame& f : log_) {
        if (f.tick != tick) continue;
        f = frame;
        f.tick = tick;
        return rewind_and_resimulate(engine, tick, error);
    }
    error = "no recorded input for that tick";
    return false;
}

}  // namespace unify
