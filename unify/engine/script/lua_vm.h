#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct lua_State;

namespace unify {
class BinaryWriter;
class BinaryReader;
class Rng;

/// Owns a Lua 5.4 state whose memory is accounted and capped separately from the engine
/// heap, and whose garbage collector runs only when the engine schedules it.
class LuaVM {
public:
    explicit LuaVM(size_t memory_limit = 64u << 20);
    ~LuaVM();
    LuaVM(const LuaVM&) = delete;
    LuaVM& operator=(const LuaVM&) = delete;

    lua_State* L() const { return L_; }

    /// Deterministic sandbox: math.random uses the engine RNG; os/io/debug/loadfile are removed.
    void sandbox(Rng* rng);
    bool run(const std::string& code, const std::string& chunk_name, int results = 0);
    const std::string& last_error() const { return error_; }
    void set_error(std::string e) { error_ = std::move(e); }
    /// Calls global (or table-field) function `name` if present. Returns false on Lua error.
    bool call_global(const char* name, int nargs = 0, int nresults = 0);

    // ---- GC scheduling ------------------------------------------------------------------
    void gc_step();                    // runs one budgeted incremental step (unless paused)
    void gc_budget(int kb_per_step);   // work per gc_step, in KB of allocation debt
    void gc_pause();                   // no collection at all until gc_resume()
    void gc_resume();
    void gc_collect();                 // full collection (e.g. during a loading screen)
    int gc_budget_kb() const { return budget_kb_; }
    bool gc_paused() const { return paused_; }

    size_t bytes() const { return used_; }
    size_t peak_bytes() const { return peak_; }
    size_t limit() const { return limit_; }
    uint64_t allocation_failures() const { return failures_; }

    /// Serializes a Lua value (nil/bool/number/string/table; no functions or cycles).
    /// Entity userdata are written as handles via the supplied tag.
    bool serialize_value(int index, BinaryWriter& w, std::string& error);
    bool deserialize_value(BinaryReader& r, std::string& error);

private:
    static void* alloc(void* ud, void* ptr, size_t osize, size_t nsize);
    lua_State* L_ = nullptr;
    size_t used_ = 0, peak_ = 0, limit_;
    uint64_t failures_ = 0;
    int budget_kb_ = 64;
    bool paused_ = false;
    std::string error_;
};

/// Name of the metatable used for entity userdata (holds an EntityHandle's bits only).
extern const char* const kEntityMeta;
extern const char* const kBufferMeta;   // userdata: LuaBuffer
extern const char* const kKernelMeta;   // userdata: uint32_t KernelHandle bits
struct LuaBuffer { uint32_t bits; bool is_int; };

}  // namespace unify
