// WebAssembly interpreter. Executes decoded modules directly from their bytecode using
// precomputed block side tables. Every failure mode is a trap returned to the caller.
#include "wasm.h"
#include <cmath>
#include <cstring>
#include <limits>

namespace unify::wasm {

namespace {
// Portable integer bit operations for GCC, Clang and MSVC.
inline uint32_t leading_zeros(uint32_t x) { uint32_t n=32; for (;x;x>>=1) --n; return n; }
inline uint32_t trailing_zeros(uint32_t x) { if (!x) return 32; uint32_t n=0; while (!(x&1)) {++n;x>>=1;} return n; }
inline uint32_t population(uint32_t x) { uint32_t n=0; while (x) {x&=x-1;++n;} return n; }
inline uint32_t f2b(float f) { uint32_t b; std::memcpy(&b, &f, 4); return b; }
inline float b2f(uint64_t b) { uint32_t x = uint32_t(b); float f; std::memcpy(&f, &x, 4); return f; }
inline uint64_t d2b(double d) { uint64_t b; std::memcpy(&b, &d, 8); return b; }
inline double b2d(uint64_t b) { double d; std::memcpy(&d, &b, 8); return d; }

struct Code {
    const uint8_t* base;
    const uint8_t* p;
    uint32_t pos() const { return uint32_t(p - base); }
    uint64_t uleb() { uint64_t r = 0; int s = 0; uint8_t b; do { b = *p++; r |= uint64_t(b & 0x7F) << s; s += 7; } while (b & 0x80); return r; }
    int64_t sleb() { int64_t r = 0; int s = 0; uint8_t b; do { b = *p++; r |= int64_t(b & 0x7F) << s; s += 7; } while (b & 0x80); if (s < 64 && (b & 0x40)) r |= -(int64_t(1) << s); return r; }
};

template <typename F> F wasm_min(F a, F b) {
    if (std::isnan(a) || std::isnan(b)) return std::numeric_limits<F>::quiet_NaN();
    if (a == b) return std::signbit(a) ? a : b;  // min(-0, +0) = -0
    return a < b ? a : b;
}
template <typename F> F wasm_max(F a, F b) {
    if (std::isnan(a) || std::isnan(b)) return std::numeric_limits<F>::quiet_NaN();
    if (a == b) return std::signbit(a) ? b : a;
    return a > b ? a : b;
}

struct Label { uint32_t cont; uint32_t height; uint32_t arity; bool loop; };
}  // namespace

const FuncType& Instance::type_of(uint32_t idx) const {
    if (idx < module_->imported_funcs) {
        uint32_t k = 0;
        for (auto& im : module_->imports) if (im.kind == 0 && k++ == idx) return module_->types[im.type_index];
    }
    return module_->types[module_->functions[idx - module_->imported_funcs].type_index];
}

bool Instance::instantiate(std::shared_ptr<const Module> module, std::shared_ptr<Memory> shared_memory,
                           const std::map<std::string, HostFunc>& host_funcs, std::string& error) {
    module_ = std::move(module);
    host_.clear();
    for (auto& im : module_->imports) {
        if (im.kind != 0) continue;
        auto it = host_funcs.find(im.module + "." + im.name);
        if (it == host_funcs.end()) { error = "unresolved import " + im.module + "." + im.name; return false; }
        host_.push_back(it->second);
    }
    if (module_->memory_imported) {
        if (!shared_memory) { error = "module imports a memory but none was provided"; return false; }
        if (shared_memory->pages() < module_->memory_min) { error = "provided memory is smaller than the import minimum"; return false; }
        memory_ = shared_memory;
    } else if (module_->has_memory) {
        memory_ = std::make_shared<Memory>();
        memory_->max_pages = module_->memory_has_max ? module_->memory_max : 65536;
        memory_->bytes.assign(size_t(module_->memory_min) * Memory::kPage, 0);
    }
    for (auto& d : module_->data) {
        if (!memory_ || uint64_t(d.offset) + d.bytes.size() > memory_->bytes.size()) { error = "data segment out of bounds"; return false; }
        std::memcpy(memory_->bytes.data() + d.offset, d.bytes.data(), d.bytes.size());
    }
    globals_.clear();
    for (auto& g : module_->globals) globals_.push_back(g.init);
    return true;
}

bool Instance::has_export(const std::string& name) const {
    for (auto& e : module_->exports) if (e.kind == 0 && e.name == name) return true;
    return false;
}

ExecResult Instance::call(const std::string& name, const std::vector<Value>& args) {
    for (auto& e : module_->exports)
        if (e.kind == 0 && e.name == name) return call_index(e.index, args);
    ExecResult r;
    r.ok = false;
    r.trap = "no exported function '" + name + "'";
    return r;
}

ExecResult Instance::call_index(uint32_t idx, const std::vector<Value>& args) {
    ExecResult r;
    uint32_t total = module_->imported_funcs + uint32_t(module_->functions.size());
    if (idx >= total) { r.ok = false; r.trap = "function index out of range"; return r; }
    const FuncType& t = type_of(idx);
    if (args.size() != t.params.size()) { r.ok = false; r.trap = "argument count mismatch"; return r; }
    for (size_t i = 0; i < args.size(); i++)
        if (args[i].type != t.params[i]) { r.ok = false; r.trap = "argument " + std::to_string(i) + " has the wrong type"; return r; }
    std::vector<uint64_t> stack;
    stack.reserve(1024);
    for (auto& a : args) stack.push_back(a.bits);
    r.ok = run(idx, stack, r.trap, r.instructions, 0);
    if (r.ok) for (size_t i = 0; i < t.results.size(); i++) r.results.push_back({t.results[i], stack[stack.size() - t.results.size() + i]});
    return r;
}

bool Instance::run(uint32_t idx, std::vector<uint64_t>& stack, std::string& trap, uint64_t& executed, uint32_t depth) {
    if (depth > max_call_depth) { trap = "call stack exhausted"; return false; }
    const FuncType& ft = type_of(idx);
    if (idx < module_->imported_funcs) {
        std::vector<Value> args(ft.params.size()), results;
        for (size_t i = ft.params.size(); i-- > 0;) { args[i] = {ft.params[i], stack.back()}; stack.pop_back(); }
        if (!host_[idx](args, results, trap)) return false;
        if (results.size() != ft.results.size()) { trap = "host function returned wrong result count"; return false; }
        for (auto& v : results) stack.push_back(v.bits);
        return true;
    }
    const Function& fn = module_->functions[idx - module_->imported_funcs];
    std::vector<uint64_t> locals(ft.params.size() + fn.locals.size(), 0);
    for (size_t i = ft.params.size(); i-- > 0;) { locals[i] = stack.back(); stack.pop_back(); }
    const uint32_t base_height = uint32_t(stack.size());
    std::vector<Label> labels;
    labels.reserve(16);
    labels.push_back({uint32_t(fn.code.size()), base_height, uint32_t(ft.results.size()), false});

    Memory* mem = memory_.get();
    Code c{fn.code.data(), fn.code.data()};
    const uint8_t* end = fn.code.data() + fn.code.size();

    auto block_arity = [&](Code& cc) -> uint32_t {
        uint8_t b = *cc.p;
        if (b == 0x40) { cc.p++; return 0; }
        if (b == 0x7F || b == 0x7E || b == 0x7D || b == 0x7C) { cc.p++; return 1; }
        int64_t ti = cc.sleb();
        return uint32_t(module_->types[size_t(ti)].results.size());
    };
    // Branch to label `depth` (0 = innermost). Returns false if this exits the function.
    auto branch = [&](uint32_t d) -> bool {
        Label target = labels[labels.size() - 1 - d];
        uint32_t arity = target.loop ? 0 : target.arity;
        for (uint32_t i = 0; i < arity; i++) stack[target.height + i] = stack[stack.size() - arity + i];
        stack.resize(target.height + arity);
        if (target.loop) {
            labels.resize(labels.size() - d);
            c.p = c.base + target.cont;
            return true;
        }
        labels.resize(labels.size() - 1 - d);
        c.p = c.base + target.cont;
        return !labels.empty();
    };
    auto pop = [&stack]() { uint64_t v = stack.back(); stack.pop_back(); return v; };
#define POP() pop()
#define TOP() stack.back()
#define TRAP(msg) do { trap = msg; return false; } while (0)
#define MEMCHECK(addr, n) if (!mem || uint64_t(addr) + (n) > mem->bytes.size()) TRAP("out of bounds memory access")

    while (c.p < end) {
        if (++executed > fuel) TRAP("fuel exhausted");
        uint32_t at = c.pos();
        uint8_t op = *c.p++;
        switch (op) {
            case 0x00: TRAP("unreachable executed");
            case 0x01: break;
            case 0x02: { uint32_t ar = block_arity(c); labels.push_back({fn.end_of.at(at) + 1, uint32_t(stack.size()), ar, false}); break; }
            case 0x03: { block_arity(c); labels.push_back({c.pos(), uint32_t(stack.size()), 0, true}); break; }
            case 0x04: {
                uint32_t ar = block_arity(c);
                uint32_t cond = uint32_t(POP());
                uint32_t e = fn.end_of.at(at);
                auto el = fn.else_of.find(at);
                if (cond) labels.push_back({e + 1, uint32_t(stack.size()), ar, false});
                else if (el != fn.else_of.end()) { labels.push_back({e + 1, uint32_t(stack.size()), ar, false}); c.p = c.base + el->second + 1; }
                else c.p = c.base + e + 1;
                break;
            }
            case 0x05: { Label l = labels.back(); labels.pop_back(); c.p = c.base + l.cont; break; }  // end of then-branch
            case 0x0B:
                labels.pop_back();
                if (labels.empty()) goto done;
                break;
            case 0x0C: if (!branch(uint32_t(c.uleb()))) goto done; break;
            case 0x0D: { uint32_t d = uint32_t(c.uleb()); if (uint32_t(POP())) { if (!branch(d)) goto done; } break; }
            case 0x0E: {
                uint64_t n = c.uleb();
                std::vector<uint32_t> targets(n + 1);
                for (auto& t : targets) t = uint32_t(c.uleb());
                uint32_t i = uint32_t(POP());
                if (!branch(targets[i < n ? i : n])) goto done;
                break;
            }
            case 0x0F: if (!branch(uint32_t(labels.size() - 1))) goto done; goto done;
            case 0x10: { uint32_t f = uint32_t(c.uleb()); if (!run(f, stack, trap, executed, depth + 1)) return false; break; }
            case 0x11: {
                uint32_t ti = uint32_t(c.uleb()); c.uleb();
                uint32_t i = uint32_t(POP());
                if (i >= module_->table.size() || module_->table[i] == 0xFFFFFFFFu) TRAP("undefined table element");
                if (!(type_of(module_->table[i]) == module_->types[ti])) TRAP("indirect call type mismatch");
                if (!run(module_->table[i], stack, trap, executed, depth + 1)) return false;
                break;
            }
            case 0x1A: stack.pop_back(); break;
            case 0x1B: { uint32_t cond = uint32_t(POP()); uint64_t b = POP(); uint64_t a = POP(); stack.push_back(cond ? a : b); break; }
            case 0x20: stack.push_back(locals[c.uleb()]); break;
            case 0x21: locals[c.uleb()] = POP(); break;
            case 0x22: locals[c.uleb()] = TOP(); break;
            case 0x23: stack.push_back(globals_[c.uleb()].bits); break;
            case 0x24: globals_[c.uleb()].bits = POP(); break;

            // ---- memory
            case 0x28: case 0x29: case 0x2A: case 0x2B: case 0x2C: case 0x2D: case 0x2E: case 0x2F:
            case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: case 0x35: {
                c.uleb();
                uint64_t addr = uint64_t(uint32_t(POP())) + c.uleb();
                static const uint8_t sizes[] = {4, 8, 4, 8, 1, 1, 2, 2, 1, 1, 2, 2, 4, 4};
                uint8_t n = sizes[op - 0x28];
                MEMCHECK(addr, n);
                uint64_t raw = 0;
                std::memcpy(&raw, mem->bytes.data() + addr, n);
                uint64_t v = raw;
                switch (op) {
                    case 0x2C: v = uint32_t(int32_t(int8_t(raw))); break;
                    case 0x2E: v = uint32_t(int32_t(int16_t(raw))); break;
                    case 0x30: v = uint64_t(int64_t(int8_t(raw))); break;
                    case 0x32: v = uint64_t(int64_t(int16_t(raw))); break;
                    case 0x34: v = uint64_t(int64_t(int32_t(raw))); break;
                    default: break;
                }
                stack.push_back(v);
                break;
            }
            case 0x36: case 0x37: case 0x38: case 0x39: case 0x3A: case 0x3B: case 0x3C: case 0x3D: case 0x3E: {
                c.uleb();
                uint64_t off = c.uleb();
                uint64_t v = POP();
                uint64_t addr = uint64_t(uint32_t(POP())) + off;
                static const uint8_t sizes[] = {4, 8, 4, 8, 1, 2, 1, 2, 4};
                uint8_t n = sizes[op - 0x36];
                MEMCHECK(addr, n);
                std::memcpy(mem->bytes.data() + addr, &v, n);
                break;
            }
            case 0x3F: c.p++; stack.push_back(mem ? mem->pages() : 0); break;
            case 0x40: { c.p++; uint32_t d = uint32_t(POP()); stack.push_back(uint32_t(mem ? mem->grow(d) : -1)); break; }

            case 0x41: stack.push_back(uint32_t(int32_t(c.sleb()))); break;
            case 0x42: stack.push_back(uint64_t(c.sleb())); break;
            case 0x43: { uint32_t b; std::memcpy(&b, c.p, 4); c.p += 4; stack.push_back(b); break; }
            case 0x44: { uint64_t b; std::memcpy(&b, c.p, 8); c.p += 8; stack.push_back(b); break; }

            // ---- i32 comparisons
            case 0x45: TOP() = uint32_t(TOP()) == 0; break;
#define I32CMP(opc, expr) case opc: { uint32_t b = uint32_t(POP()), a = uint32_t(POP()); (void)a; (void)b; stack.push_back((expr) ? 1 : 0); break; }
            I32CMP(0x46, a == b) I32CMP(0x47, a != b) I32CMP(0x48, int32_t(a) < int32_t(b)) I32CMP(0x49, a < b)
            I32CMP(0x4A, int32_t(a) > int32_t(b)) I32CMP(0x4B, a > b) I32CMP(0x4C, int32_t(a) <= int32_t(b)) I32CMP(0x4D, a <= b)
            I32CMP(0x4E, int32_t(a) >= int32_t(b)) I32CMP(0x4F, a >= b)
            // ---- i64 comparisons
            case 0x50: TOP() = TOP() == 0; break;
#define I64CMP(opc, expr) case opc: { uint64_t b = POP(), a = POP(); stack.push_back((expr) ? 1 : 0); break; }
            I64CMP(0x51, a == b) I64CMP(0x52, a != b) I64CMP(0x53, int64_t(a) < int64_t(b)) I64CMP(0x54, a < b)
            I64CMP(0x55, int64_t(a) > int64_t(b)) I64CMP(0x56, a > b) I64CMP(0x57, int64_t(a) <= int64_t(b)) I64CMP(0x58, a <= b)
            I64CMP(0x59, int64_t(a) >= int64_t(b)) I64CMP(0x5A, a >= b)
            // ---- f32 / f64 comparisons
#define F32CMP(opc, expr) case opc: { float b = b2f(POP()), a = b2f(POP()); stack.push_back((expr) ? 1 : 0); break; }
            F32CMP(0x5B, a == b) F32CMP(0x5C, a != b) F32CMP(0x5D, a < b) F32CMP(0x5E, a > b) F32CMP(0x5F, a <= b) F32CMP(0x60, a >= b)
#define F64CMP(opc, expr) case opc: { double b = b2d(POP()), a = b2d(POP()); stack.push_back((expr) ? 1 : 0); break; }
            F64CMP(0x61, a == b) F64CMP(0x62, a != b) F64CMP(0x63, a < b) F64CMP(0x64, a > b) F64CMP(0x65, a <= b) F64CMP(0x66, a >= b)

            // ---- i32 arithmetic
            case 0x67: { uint32_t a = uint32_t(TOP()); TOP() = a ? leading_zeros(a) : 32; break; }
            case 0x68: { uint32_t a = uint32_t(TOP()); TOP() = a ? trailing_zeros(a) : 32; break; }
            case 0x69: TOP() = population(uint32_t(TOP())); break;
#define I32BIN(opc, expr) case opc: { uint32_t b = uint32_t(POP()), a = uint32_t(POP()); stack.push_back(uint32_t(expr)); break; }
            I32BIN(0x6A, a + b) I32BIN(0x6B, a - b) I32BIN(0x6C, a * b)
            case 0x6D: { int32_t b = int32_t(POP()), a = int32_t(POP()); if (b == 0) TRAP("integer divide by zero"); if (a == INT32_MIN && b == -1) TRAP("integer overflow"); stack.push_back(uint32_t(a / b)); break; }
            case 0x6E: { uint32_t b = uint32_t(POP()), a = uint32_t(POP()); if (b == 0) TRAP("integer divide by zero"); stack.push_back(a / b); break; }
            case 0x6F: { int32_t b = int32_t(POP()), a = int32_t(POP()); if (b == 0) TRAP("integer divide by zero"); stack.push_back(uint32_t(b == -1 ? 0 : a % b)); break; }
            case 0x70: { uint32_t b = uint32_t(POP()), a = uint32_t(POP()); if (b == 0) TRAP("integer divide by zero"); stack.push_back(a % b); break; }
            I32BIN(0x71, a & b) I32BIN(0x72, a | b) I32BIN(0x73, a ^ b) I32BIN(0x74, a << (b & 31))
            I32BIN(0x75, uint32_t(int32_t(a) >> (b & 31))) I32BIN(0x76, a >> (b & 31))
            I32BIN(0x77, (a << (b & 31)) | (a >> ((32 - (b & 31)) & 31))) I32BIN(0x78, (a >> (b & 31)) | (a << ((32 - (b & 31)) & 31)))
            // ---- i64 arithmetic (common subset)
#define I64BIN(opc, expr) case opc: { uint64_t b = POP(), a = POP(); stack.push_back(uint64_t(expr)); break; }
            I64BIN(0x7C, a + b) I64BIN(0x7D, a - b) I64BIN(0x7E, a * b)
            case 0x7F: { int64_t b = int64_t(POP()), a = int64_t(POP()); if (b == 0) TRAP("integer divide by zero"); if (a == INT64_MIN && b == -1) TRAP("integer overflow"); stack.push_back(uint64_t(a / b)); break; }
            case 0x80: { uint64_t b = POP(), a = POP(); if (b == 0) TRAP("integer divide by zero"); stack.push_back(a / b); break; }
            I64BIN(0x83, a & b) I64BIN(0x84, a | b) I64BIN(0x85, a ^ b) I64BIN(0x86, a << (b & 63))
            I64BIN(0x87, uint64_t(int64_t(a) >> (b & 63))) I64BIN(0x88, a >> (b & 63))

            // ---- f32 arithmetic
#define F32UN(opc, expr) case opc: { float a = b2f(TOP()); TOP() = f2b(expr); break; }
            F32UN(0x8B, std::fabs(a)) F32UN(0x8C, -a) F32UN(0x8D, std::ceil(a)) F32UN(0x8E, std::floor(a))
            F32UN(0x8F, std::trunc(a)) F32UN(0x90, std::nearbyint(a)) F32UN(0x91, std::sqrt(a))
#define F32BIN(opc, expr) case opc: { float b = b2f(POP()), a = b2f(POP()); stack.push_back(f2b(expr)); break; }
            F32BIN(0x92, a + b) F32BIN(0x93, a - b) F32BIN(0x94, a * b) F32BIN(0x95, a / b)
            F32BIN(0x96, wasm_min(a, b)) F32BIN(0x97, wasm_max(a, b)) F32BIN(0x98, std::copysign(a, b))
            // ---- f64 arithmetic
#define F64UN(opc, expr) case opc: { double a = b2d(TOP()); TOP() = d2b(expr); break; }
            F64UN(0x99, std::fabs(a)) F64UN(0x9A, -a) F64UN(0x9B, std::ceil(a)) F64UN(0x9C, std::floor(a))
            F64UN(0x9D, std::trunc(a)) F64UN(0x9E, std::nearbyint(a)) F64UN(0x9F, std::sqrt(a))
#define F64BIN(opc, expr) case opc: { double b = b2d(POP()), a = b2d(POP()); stack.push_back(d2b(expr)); break; }
            F64BIN(0xA0, a + b) F64BIN(0xA1, a - b) F64BIN(0xA2, a * b) F64BIN(0xA3, a / b)
            F64BIN(0xA4, wasm_min(a, b)) F64BIN(0xA5, wasm_max(a, b)) F64BIN(0xA6, std::copysign(a, b))

            // ---- conversions
            case 0xA7: TOP() = uint32_t(TOP()); break;                                   // i32.wrap_i64
            case 0xA8: case 0xA9: {                                                      // i32.trunc_f32_s/u
                float a = b2f(TOP());
                if (std::isnan(a)) TRAP("invalid conversion to integer");
                if (op == 0xA8 ? (a < -2147483648.0f || a >= 2147483648.0f) : (a <= -1.0f || a >= 4294967296.0f)) TRAP("integer overflow");
                TOP() = op == 0xA8 ? uint32_t(int32_t(a)) : uint32_t(a);
                break;
            }
            case 0xAC: TOP() = uint64_t(int64_t(int32_t(TOP()))); break;                  // i64.extend_i32_s
            case 0xAD: TOP() = uint64_t(uint32_t(TOP())); break;                          // i64.extend_i32_u
            case 0xB2: TOP() = f2b(float(int32_t(TOP()))); break;                         // f32.convert_i32_s
            case 0xB3: TOP() = f2b(float(uint32_t(TOP()))); break;                        // f32.convert_i32_u
            case 0xB4: TOP() = f2b(float(int64_t(TOP()))); break;
            case 0xB6: TOP() = f2b(float(b2d(TOP()))); break;                             // f32.demote_f64
            case 0xB7: TOP() = d2b(double(int32_t(TOP()))); break;
            case 0xB8: TOP() = d2b(double(uint32_t(TOP()))); break;
            case 0xB9: TOP() = d2b(double(int64_t(TOP()))); break;
            case 0xBB: TOP() = d2b(double(b2f(TOP()))); break;                            // f64.promote_f32
            case 0xBC: TOP() = uint32_t(TOP()); break;                                   // i32.reinterpret_f32 (bits already)
            case 0xBD: case 0xBE: case 0xBF: break;                                       // other reinterprets: bits unchanged
            default: TRAP("unsupported opcode 0x" + std::to_string(op) + " at " + std::to_string(at));
        }
    }
done:
    // Leave exactly the function's results above the caller's stack.
    {
        uint32_t nres = uint32_t(ft.results.size());
        if (stack.size() < base_height + nres) TRAP("stack underflow at function exit");
        for (uint32_t i = 0; i < nres; i++) stack[base_height + i] = stack[stack.size() - nres + i];
        stack.resize(base_height + nres);
    }
    return true;
#undef POP
#undef TOP
#undef TRAP
#undef MEMCHECK
}

}  // namespace unify::wasm
