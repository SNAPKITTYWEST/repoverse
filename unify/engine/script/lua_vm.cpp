#include "lua_vm.h"
#include "../core/core.h"
#include "../serialization/stream.h"
#include <cstdlib>
#include <cmath>
#include <algorithm>
// Lua is compiled as C++ (see CMakeLists.txt) so script errors unwind with exceptions,
// which runs destructors in binding code instead of longjmp-ing over them.
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

namespace unify {

const char* const kEntityMeta = "unify.Entity";
const char* const kBufferMeta = "unify.Buffer";
const char* const kKernelMeta = "unify.Kernel";

void* LuaVM::alloc(void* ud, void* ptr, size_t osize, size_t nsize) {
    auto* vm = static_cast<LuaVM*>(ud);
    if (ptr == nullptr) osize = 0;  // osize encodes the object type when ptr is NULL
    if (nsize == 0) {
        vm->used_ -= osize;
        std::free(ptr);
        return nullptr;
    }
    if (vm->used_ - osize + nsize > vm->limit_) { vm->failures_++; return nullptr; }  // Lua raises "not enough memory"
    void* p = std::realloc(ptr, nsize);
    if (!p) { vm->failures_++; return nullptr; }
    vm->used_ = vm->used_ - osize + nsize;
    if (vm->used_ > vm->peak_) vm->peak_ = vm->used_;
    return p;
}

LuaVM::LuaVM(size_t memory_limit) : limit_(memory_limit) {
    L_ = lua_newstate(&LuaVM::alloc, this);
    luaL_openlibs(L_);
    // The engine owns collection timing: stop the automatic collector and use
    // incremental steps scheduled once per tick with a fixed budget.
    lua_gc(L_, LUA_GCINC, 0, 0, 0);
    lua_gc(L_, LUA_GCSTOP);
}

LuaVM::~LuaVM() { if (L_) lua_close(L_); }

static int deterministic_random(lua_State* L) {
    Rng* rng = static_cast<Rng*>(lua_touserdata(L, lua_upvalueindex(1)));
    int n = lua_gettop(L);
    if (n == 0) { lua_pushnumber(L, double(rng->next_float())); return 1; }
    lua_Integer lo = 1, hi;
    if (n == 1) hi = luaL_checkinteger(L, 1);
    else { lo = luaL_checkinteger(L, 1); hi = luaL_checkinteger(L, 2); }
    luaL_argcheck(L, lo <= hi, n, "interval is empty");
    lua_pushinteger(L, rng->range_int(int32_t(lo), int32_t(hi)));
    return 1;
}

void LuaVM::sandbox(Rng* rng) {
    // Nondeterministic or unsafe services are not reachable from gameplay code.
    for (const char* g : {"os", "io", "debug", "dofile", "loadfile", "require", "package", "collectgarbage"}) {
        lua_pushnil(L_);
        lua_setglobal(L_, g);
    }
    lua_getglobal(L_, "math");
    lua_pushlightuserdata(L_, rng);
    lua_pushcclosure(L_, deterministic_random, 1);
    lua_setfield(L_, -2, "random");
    lua_pushnil(L_);
    lua_setfield(L_, -2, "randomseed");
    lua_pop(L_, 1);
}

bool LuaVM::run(const std::string& code, const std::string& chunk_name, int results) {
    if (luaL_loadbuffer(L_, code.data(), code.size(), ("@" + chunk_name).c_str()) != LUA_OK) {
        error_ = lua_tostring(L_, -1);
        lua_pop(L_, 1);
        return false;
    }
    if (lua_pcall(L_, 0, results, 0) != LUA_OK) {
        error_ = lua_tostring(L_, -1);
        lua_pop(L_, 1);
        return false;
    }
    return true;
}

bool LuaVM::call_global(const char* name, int nargs, int nresults) {
    lua_getglobal(L_, name);
    if (!lua_isfunction(L_, -1)) { lua_pop(L_, 1 + nargs); return true; }
    if (nargs) lua_insert(L_, -(nargs + 1));
    if (lua_pcall(L_, nargs, nresults, 0) != LUA_OK) {
        error_ = lua_tostring(L_, -1);
        lua_pop(L_, 1);
        return false;
    }
    return true;
}

void LuaVM::gc_step() { if (!paused_) lua_gc(L_, LUA_GCSTEP, budget_kb_); }
void LuaVM::gc_budget(int kb) { budget_kb_ = kb < 1 ? 1 : kb; }
void LuaVM::gc_pause() { paused_ = true; }
void LuaVM::gc_resume() { paused_ = false; }
void LuaVM::gc_collect() { lua_gc(L_, LUA_GCCOLLECT); lua_gc(L_, LUA_GCSTOP); }

// ------------------------------------------------------------------------------ serialization
// Tags: 0 nil, 1 false, 2 true, 3 integer, 4 float, 5 string, 6 table, 7 entity, 8 buffer, 9 kernel.
// Handles serialize as handle bits; the owning subsystems restore the same handles.
static bool write_value(lua_State* L, int idx, BinaryWriter& w, std::string& err, int depth) {
    if (depth > 64) { err = "table nesting too deep (cycle?)"; return false; }
    idx = lua_absindex(L, idx);
    switch (lua_type(L, idx)) {
        case LUA_TNIL: w.u8(0); return true;
        case LUA_TBOOLEAN: w.u8(lua_toboolean(L, idx) ? 2 : 1); return true;
        case LUA_TNUMBER:
            if (lua_isinteger(L, idx)) { w.u8(3); w.u64(uint64_t(lua_tointeger(L, idx))); }
            else { w.u8(4); w.f64(lua_tonumber(L, idx)); }
            return true;
        case LUA_TSTRING: { size_t n; const char* s = lua_tolstring(L, idx, &n); w.u8(5); w.str(std::string(s, n)); return true; }
        case LUA_TUSERDATA:
            if (luaL_testudata(L, idx, kEntityMeta)) { w.u8(7); w.u32(*static_cast<uint32_t*>(lua_touserdata(L, idx))); return true; }
            if (luaL_testudata(L, idx, kBufferMeta)) { auto* b = static_cast<LuaBuffer*>(lua_touserdata(L, idx)); w.u8(8); w.u32(b->bits); w.boolean(b->is_int); return true; }
            if (luaL_testudata(L, idx, kKernelMeta)) { w.u8(9); w.u32(*static_cast<uint32_t*>(lua_touserdata(L, idx))); return true; }
            err = "cannot serialize userdata";
            return false;
        case LUA_TTABLE: {
            // Deterministic key order: collect keys, sort (numbers before strings).
            struct Key { bool is_num; lua_Integer i; double d; bool is_int; std::string s; };
            std::vector<Key> keys;
            lua_pushnil(L);
            while (lua_next(L, idx)) {
                lua_pop(L, 1);
                Key k{};
                if (lua_type(L, -1) == LUA_TNUMBER) { k.is_num = true; k.is_int = lua_isinteger(L, -1); k.i = lua_tointeger(L, -1); k.d = lua_tonumber(L, -1); }
                else if (lua_type(L, -1) == LUA_TSTRING) { k.s = lua_tostring(L, -1); }
                else { lua_pop(L, 1); err = "table keys must be numbers or strings"; return false; }
                keys.push_back(k);
            }
            std::sort(keys.begin(), keys.end(), [](const Key& a, const Key& b) {
                if (a.is_num != b.is_num) return a.is_num;
                if (a.is_num) return a.d < b.d;
                return a.s < b.s;
            });
            w.u8(6);
            w.u32(uint32_t(keys.size()));
            for (auto& k : keys) {
                if (k.is_num) { if (k.is_int) lua_pushinteger(L, k.i); else lua_pushnumber(L, k.d); }
                else lua_pushlstring(L, k.s.data(), k.s.size());
                if (!write_value(L, -1, w, err, depth + 1)) { lua_pop(L, 1); return false; }
                lua_gettable(L, idx);
                bool ok = write_value(L, -1, w, err, depth + 1);
                lua_pop(L, 1);
                if (!ok) return false;
            }
            return true;
        }
        default:
            err = std::string("cannot serialize a ") + lua_typename(L, lua_type(L, idx));
            return false;
    }
}

static bool read_value(lua_State* L, BinaryReader& r, std::string& err, int depth) {
    if (depth > 64) { err = "table nesting too deep"; return false; }
    luaL_checkstack(L, 4, "deserialize");
    switch (r.u8()) {
        case 0: lua_pushnil(L); return true;
        case 1: lua_pushboolean(L, 0); return true;
        case 2: lua_pushboolean(L, 1); return true;
        case 3: lua_pushinteger(L, lua_Integer(r.u64())); return true;
        case 4: lua_pushnumber(L, r.f64()); return true;
        case 5: { std::string s = r.str(); lua_pushlstring(L, s.data(), s.size()); return true; }
        case 7: {
            auto* p = static_cast<uint32_t*>(lua_newuserdatauv(L, sizeof(uint32_t), 0));
            *p = r.u32();
            luaL_setmetatable(L, kEntityMeta);
            return true;
        }
        case 8: {
            auto* b = static_cast<LuaBuffer*>(lua_newuserdatauv(L, sizeof(LuaBuffer), 0));
            b->bits = r.u32();
            b->is_int = r.boolean();
            luaL_setmetatable(L, kBufferMeta);
            return true;
        }
        case 9: {
            *static_cast<uint32_t*>(lua_newuserdatauv(L, sizeof(uint32_t), 0)) = r.u32();
            luaL_setmetatable(L, kKernelMeta);
            return true;
        }
        case 6: {
            uint32_t n = r.u32();
            lua_createtable(L, 0, int(n));
            for (uint32_t i = 0; i < n; i++) {
                if (!read_value(L, r, err, depth + 1)) return false;
                if (!read_value(L, r, err, depth + 1)) return false;
                lua_settable(L, -3);
            }
            return true;
        }
        default: err = "corrupt Lua value tag"; return false;
    }
}

bool LuaVM::serialize_value(int index, BinaryWriter& w, std::string& error) { return write_value(L_, index, w, error, 0); }
bool LuaVM::deserialize_value(BinaryReader& r, std::string& error) {
    int top = lua_gettop(L_);
    try {
        if (read_value(L_, r, error, 0)) return true;
    } catch (const SerializationError& e) {
        error = e.what();
    }
    lua_settop(L_, top);
    return false;
}

}  // namespace unify
