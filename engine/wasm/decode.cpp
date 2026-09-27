#include "wasm.h"
#include <cstring>

namespace unify::wasm {

Value Value::f32(float v) { uint32_t b; std::memcpy(&b, &v, 4); return {ValType::F32, b}; }
Value Value::f64(double v) { uint64_t b; std::memcpy(&b, &v, 8); return {ValType::F64, b}; }
float Value::as_f32() const { uint32_t b = uint32_t(bits); float v; std::memcpy(&v, &b, 4); return v; }
double Value::as_f64() const { double v; std::memcpy(&v, &bits, 8); return v; }

void write_uleb(std::vector<uint8_t>& out, uint64_t v) {
    do { uint8_t b = v & 0x7F; v >>= 7; if (v) b |= 0x80; out.push_back(b); } while (v);
}
void write_sleb(std::vector<uint8_t>& out, int64_t v) {
    bool more = true;
    while (more) {
        uint8_t b = v & 0x7F;
        v >>= 7;
        if ((v == 0 && !(b & 0x40)) || (v == -1 && (b & 0x40))) more = false; else b |= 0x80;
        out.push_back(b);
    }
}

namespace {
struct Reader {
    const uint8_t* p;
    const uint8_t* end;
    std::string err;
    bool ok() const { return err.empty(); }
    bool need(size_t n) { if (size_t(end - p) < n) { if (err.empty()) err = "unexpected end of module"; return false; } return true; }
    uint8_t u8() { return need(1) ? *p++ : 0; }
    uint64_t uleb(int max_bits = 32) {
        uint64_t r = 0; int shift = 0;
        for (;;) {
            if (!need(1)) return 0;
            uint8_t b = *p++;
            r |= uint64_t(b & 0x7F) << shift;
            shift += 7;
            if (!(b & 0x80)) break;
            if (shift >= max_bits + 7) { err = "LEB128 too long"; return 0; }
        }
        return r;
    }
    int64_t sleb(int bits) {
        int64_t r = 0; int shift = 0; uint8_t b;
        do {
            if (!need(1)) return 0;
            b = *p++;
            r |= int64_t(b & 0x7F) << shift;
            shift += 7;
            if (shift > bits + 7) { err = "LEB128 too long"; return 0; }
        } while (b & 0x80);
        if (shift < 64 && (b & 0x40)) r |= -(int64_t(1) << shift);
        return r;
    }
    std::string name() { uint64_t n = uleb(); if (!need(n)) return {}; std::string s(reinterpret_cast<const char*>(p), n); p += n; return s; }
    ValType valtype() {
        uint8_t t = u8();
        if (t != 0x7F && t != 0x7E && t != 0x7D && t != 0x7C) { err = "unsupported value type"; return ValType::I32; }
        return ValType(t);
    }
};

// Evaluates a constant initializer expression (i32/i64/f32/f64.const ... end).
Value const_expr(Reader& r) {
    Value v;
    uint8_t op = r.u8();
    switch (op) {
        case 0x41: v = Value::i32(int32_t(r.sleb(32))); break;
        case 0x42: v = Value::i64(r.sleb(64)); break;
        case 0x43: { if (r.need(4)) { float f; std::memcpy(&f, r.p, 4); r.p += 4; v = Value::f32(f); } break; }
        case 0x44: { if (r.need(8)) { double d; std::memcpy(&d, r.p, 8); r.p += 8; v = Value::f64(d); } break; }
        default: r.err = "unsupported constant expression";
    }
    if (r.u8() != 0x0B) r.err = "constant expression not terminated";
    return v;
}

// Skips one instruction's immediates. Returns false for unknown opcodes.
bool skip_immediates(Reader& r, uint8_t op) {
    switch (op) {
        case 0x02: case 0x03: case 0x04: {  // blocktype: 0x40, valtype, or s33 type index
            uint8_t b = *r.p;
            if (b == 0x40 || b == 0x7F || b == 0x7E || b == 0x7D || b == 0x7C) r.p++; else r.sleb(33);
            return true;
        }
        case 0x0C: case 0x0D: case 0x10: case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: r.uleb(); return true;
        case 0x0E: { uint64_t n = r.uleb(); for (uint64_t i = 0; i <= n; i++) r.uleb(); return true; }
        case 0x11: r.uleb(); r.uleb(); return true;
        case 0x3F: case 0x40: r.u8(); return true;
        case 0x41: r.sleb(32); return true;
        case 0x42: r.sleb(64); return true;
        case 0x43: r.need(4); r.p += 4; return true;
        case 0x44: r.need(8); r.p += 8; return true;
        default:
            if (op >= 0x28 && op <= 0x3E) { r.uleb(); r.uleb(); return true; }  // memarg
            if (op == 0x00 || op == 0x01 || op == 0x05 || op == 0x0B || op == 0x0F || op == 0x1A || op == 0x1B) return true;
            if (op >= 0x45 && op <= 0xC4) return true;  // numeric, no immediates
            if (op == 0xFC) { r.uleb(); return true; }  // saturating truncation prefix
            return false;
    }
}

// Builds block -> end / if -> else maps so branches jump in O(log n) without rescanning.
bool index_blocks(Function& f, std::string& err) {
    Reader r{f.code.data(), f.code.data() + f.code.size(), {}};
    std::vector<uint32_t> open;
    while (r.p < r.end && r.ok()) {
        uint32_t at = uint32_t(r.p - f.code.data());
        uint8_t op = r.u8();
        if (op == 0x02 || op == 0x03 || op == 0x04) open.push_back(at);
        if (op == 0x05) {
            if (open.empty()) { err = "else without if"; return false; }
            f.else_of[open.back()] = at;
        }
        if (op == 0x0B) {
            if (open.empty()) {
                if (r.p != r.end) { err = "code after function end"; return false; }
                return true;
            }
            f.end_of[open.back()] = at;
            open.pop_back();
        }
        if (!skip_immediates(r, op)) { err = "unsupported opcode 0x" + std::to_string(op); return false; }
    }
    if (!r.ok()) { err = r.err; return false; }
    err = "function body missing end";
    return false;
}
}  // namespace

bool decode(const std::vector<uint8_t>& bytes, Module& m, std::string& error) {
    m = Module{};
    Reader r{bytes.data(), bytes.data() + bytes.size(), {}};
    static const uint8_t magic[8] = {0x00, 'a', 's', 'm', 0x01, 0x00, 0x00, 0x00};
    if (bytes.size() < 8 || std::memcmp(bytes.data(), magic, 8)) { error = "not a WASM v1 module"; return false; }
    r.p += 8;
    std::vector<uint32_t> func_types;
    uint8_t last_id = 0;
    while (r.p < r.end && r.ok()) {
        uint8_t id = r.u8();
        uint64_t size = r.uleb();
        if (!r.need(size)) break;
        Reader s{r.p, r.p + size, {}};
        r.p += size;
        if (id != 0) {
            if (id < last_id) { error = "sections out of order"; return false; }
            last_id = id;
        }
        switch (id) {
            case 0: break;  // custom
            case 1: {  // type
                uint64_t n = s.uleb();
                for (uint64_t i = 0; i < n && s.ok(); i++) {
                    if (s.u8() != 0x60) { s.err = "expected func type"; break; }
                    FuncType t;
                    uint64_t np = s.uleb(); for (uint64_t k = 0; k < np; k++) t.params.push_back(s.valtype());
                    uint64_t nr = s.uleb(); for (uint64_t k = 0; k < nr; k++) t.results.push_back(s.valtype());
                    if (t.results.size() > 1) { s.err = "multi-value results unsupported"; break; }
                    m.types.push_back(t);
                }
                break;
            }
            case 2: {  // import
                uint64_t n = s.uleb();
                for (uint64_t i = 0; i < n && s.ok(); i++) {
                    Import im;
                    im.module = s.name(); im.name = s.name(); im.kind = s.u8();
                    if (im.kind == 0) { im.type_index = uint32_t(s.uleb()); m.imported_funcs++; }
                    else if (im.kind == 2) {
                        uint8_t flags = s.u8();
                        im.min_pages = uint32_t(s.uleb());
                        if (flags & 1) { im.max_pages = uint32_t(s.uleb()); im.has_max = true; }
                        m.has_memory = m.memory_imported = true;
                        m.memory_min = im.min_pages; m.memory_max = im.max_pages; m.memory_has_max = im.has_max;
                    } else { s.err = "only function and memory imports are supported"; }
                    m.imports.push_back(im);
                }
                break;
            }
            case 3: { uint64_t n = s.uleb(); for (uint64_t i = 0; i < n; i++) func_types.push_back(uint32_t(s.uleb())); break; }
            case 4: {  // table
                uint64_t n = s.uleb();
                for (uint64_t i = 0; i < n; i++) { s.u8(); uint8_t f = s.u8(); s.uleb(); if (f & 1) s.uleb(); }
                break;
            }
            case 5: {  // memory
                if (s.uleb() != 1) { s.err = "exactly one memory supported"; break; }
                uint8_t flags = s.u8();
                m.memory_min = uint32_t(s.uleb());
                if (flags & 1) { m.memory_max = uint32_t(s.uleb()); m.memory_has_max = true; }
                m.has_memory = true;
                break;
            }
            case 6: {  // global
                uint64_t n = s.uleb();
                for (uint64_t i = 0; i < n && s.ok(); i++) { Global g; g.type = s.valtype(); g.mut = s.u8() != 0; g.init = const_expr(s); m.globals.push_back(g); }
                break;
            }
            case 7: {  // export
                uint64_t n = s.uleb();
                for (uint64_t i = 0; i < n; i++) { Export e; e.name = s.name(); e.kind = s.u8(); e.index = uint32_t(s.uleb()); m.exports.push_back(e); }
                break;
            }
            case 8: s.uleb(); break;  // start (ignored: kernels have none)
            case 9: {  // elem (active, table 0, funcref indices)
                uint64_t n = s.uleb();
                for (uint64_t i = 0; i < n && s.ok(); i++) {
                    if (s.uleb() != 0) { s.err = "unsupported element segment"; break; }
                    Value off = const_expr(s);
                    uint64_t k = s.uleb();
                    uint32_t base = uint32_t(off.as_i32());
                    if (m.table.size() < base + k) m.table.resize(base + k, 0xFFFFFFFFu);
                    for (uint64_t j = 0; j < k; j++) m.table[base + j] = uint32_t(s.uleb());
                }
                break;
            }
            case 10: {  // code
                uint64_t n = s.uleb();
                if (n != func_types.size()) { s.err = "function/code count mismatch"; break; }
                for (uint64_t i = 0; i < n && s.ok(); i++) {
                    uint64_t body_size = s.uleb();
                    if (!s.need(body_size)) break;
                    Reader b{s.p, s.p + body_size, {}};
                    s.p += body_size;
                    Function f;
                    f.type_index = func_types[i];
                    uint64_t groups = b.uleb();
                    for (uint64_t g = 0; g < groups && b.ok(); g++) {
                        uint64_t count = b.uleb();
                        if (count > 50000) { b.err = "too many locals"; break; }
                        ValType t = b.valtype();
                        for (uint64_t k = 0; k < count; k++) f.locals.push_back(t);
                    }
                    if (!b.ok()) { s.err = b.err; break; }
                    f.code.assign(b.p, b.end);
                    std::string e;
                    if (!index_blocks(f, e)) { s.err = "function " + std::to_string(i) + ": " + e; break; }
                    m.functions.push_back(std::move(f));
                }
                break;
            }
            case 11: {  // data (active, memory 0)
                uint64_t n = s.uleb();
                for (uint64_t i = 0; i < n && s.ok(); i++) {
                    if (s.uleb() != 0) { s.err = "unsupported data segment"; break; }
                    DataSegment d;
                    d.offset = uint32_t(const_expr(s).as_i32());
                    uint64_t len = s.uleb();
                    if (!s.need(len)) break;
                    d.bytes.assign(s.p, s.p + len);
                    s.p += len;
                    m.data.push_back(std::move(d));
                }
                break;
            }
            case 12: s.uleb(); break;  // data count
            default: error = "unknown section " + std::to_string(id); return false;
        }
        if (!s.ok()) { error = s.err; return false; }
    }
    if (!r.ok()) { error = r.err; return false; }
    for (auto& f : m.functions) if (f.type_index >= m.types.size()) { error = "function type index out of range"; return false; }
    for (auto& im : m.imports) if (im.kind == 0 && im.type_index >= m.types.size()) { error = "import type index out of range"; return false; }
    return true;
}

}  // namespace unify::wasm
