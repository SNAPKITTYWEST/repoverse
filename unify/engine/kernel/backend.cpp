// IR optimisation (constant folding), IR printing, and WASM code generation.
#include "kernel.h"
#include "../wasm/wasm.h"
#include <cmath>
#include <cstring>
#include <sstream>

namespace unify::kernel {

namespace {
float as_f(uint32_t b) { float f; std::memcpy(&f, &b, 4); return f; }
uint32_t fb(float f) { uint32_t b; std::memcpy(&b, &f, 4); return b; }

// ---------------------------------------------------------------------------- folding
bool fold_expr(IRExprP& e, uint32_t& count) {
    for (auto& a : e->args) fold_expr(a, count);
    auto is_const = [](const IRExprP& x) { return x->kind == IRExpr::Const; };
    if (e->kind == IRExpr::Convert && is_const(e->args[0])) {
        IRExpr& a = *e->args[0];
        uint32_t bits;
        if (e->type == IRType::F32) bits = fb(float(int32_t(a.bits)));
        else {
            float f = as_f(a.bits);
            if (!(f > -2147483648.0f && f < 2147483648.0f)) return false;  // would trap at runtime; keep it
            bits = uint32_t(int32_t(f));
        }
        auto c = std::make_unique<IRExpr>();
        c->kind = IRExpr::Const; c->type = e->type; c->bits = bits;
        e = std::move(c);
        count++;
        return true;
    }
    if (e->kind == IRExpr::Binary && is_const(e->args[0]) && is_const(e->args[1])) {
        const std::string& op = e->op;
        IRType operand = e->args[0]->type;
        uint32_t r = 0;
        if (operand == IRType::I32) {
            int32_t a = int32_t(e->args[0]->bits), b = int32_t(e->args[1]->bits);
            if ((op == "/" || op == "%") && (b == 0 || (a == INT32_MIN && b == -1))) return false;  // runtime trap semantics
            if (op == "+") r = uint32_t(a) + uint32_t(b);
            else if (op == "-") r = uint32_t(a) - uint32_t(b);
            else if (op == "*") r = uint32_t(a) * uint32_t(b);
            else if (op == "/") r = uint32_t(a / b);
            else if (op == "%") r = uint32_t(a % b);
            else if (op == "<") r = a < b; else if (op == "<=") r = a <= b;
            else if (op == ">") r = a > b; else if (op == ">=") r = a >= b;
            else if (op == "==") r = a == b; else if (op == "!=") r = a != b;
            else if (op == "&&") r = (a != 0) & (b != 0); else if (op == "||") r = (a != 0) | (b != 0);
            else return false;
        } else {
            float a = as_f(e->args[0]->bits), b = as_f(e->args[1]->bits);
            if (op == "+") r = fb(a + b); else if (op == "-") r = fb(a - b);
            else if (op == "*") r = fb(a * b); else if (op == "/") r = fb(a / b);
            else if (op == "<") r = a < b; else if (op == "<=") r = a <= b;
            else if (op == ">") r = a > b; else if (op == ">=") r = a >= b;
            else if (op == "==") r = a == b; else if (op == "!=") r = a != b;
            else return false;
        }
        auto c = std::make_unique<IRExpr>();
        c->kind = IRExpr::Const; c->type = e->type; c->bits = r;
        e = std::move(c);
        count++;
        return true;
    }
    if (e->kind == IRExpr::Unary && is_const(e->args[0])) {
        uint32_t v = e->args[0]->bits, r;
        if (e->op == "!") r = v == 0;
        else if (e->type == IRType::I32) r = uint32_t(-int32_t(v));
        else r = fb(-as_f(v));
        auto c = std::make_unique<IRExpr>();
        c->kind = IRExpr::Const; c->type = e->type; c->bits = r;
        e = std::move(c);
        count++;
        return true;
    }
    return false;
}

void fold_block(std::vector<IRStmtP>& body, uint32_t& count) {
    std::vector<IRStmtP> out;
    for (auto& s : body) {
        if (s->value) fold_expr(s->value, count);
        if (s->index) fold_expr(s->index, count);
        if (s->cond) fold_expr(s->cond, count);
        fold_block(s->then_body, count);
        fold_block(s->else_body, count);
        fold_block(s->body, count);
        fold_block(s->step, count);
        // Dead-branch elimination for constant conditions.
        if (s->kind == IRStmt::If && s->cond->kind == IRExpr::Const) {
            auto& taken = s->cond->bits ? s->then_body : s->else_body;
            for (auto& t : taken) out.push_back(std::move(t));
            count++;
            continue;
        }
        if (s->kind == IRStmt::Loop && s->cond->kind == IRExpr::Const && s->cond->bits == 0) { count++; continue; }
        out.push_back(std::move(s));
    }
    body = std::move(out);
}

// ---------------------------------------------------------------------------- printing
const char* tn(IRType t) { return t == IRType::F32 ? "f32" : "i32"; }

void print_expr(std::ostream& o, const IRExpr& e, const IRFunction& fn) {
    auto local_name = [&](uint32_t i) -> std::string {
        static const char* b[] = {"threadIdx.x", "blockIdx.x", "blockDim.x", "gridDim.x"};
        if (i < 4) return b[i];
        if (i < fn.param_types.size()) return "%p" + std::to_string(i - 4);
        return "%" + fn.local_names[i - fn.param_types.size()];
    };
    switch (e.kind) {
        case IRExpr::Const: if (e.type == IRType::F32) o << as_f(e.bits) << "f"; else o << int32_t(e.bits); break;
        case IRExpr::Local: o << local_name(e.local); break;
        case IRExpr::Load: o << "load." << tn(e.type) << " " << local_name(e.local) << "["; print_expr(o, *e.args[0], fn); o << "]"; break;
        case IRExpr::Binary: o << "("; print_expr(o, *e.args[0], fn); o << " " << e.op << "." << tn(e.args[0]->type) << " "; print_expr(o, *e.args[1], fn); o << ")"; break;
        case IRExpr::Unary: o << e.op << "("; print_expr(o, *e.args[0], fn); o << ")"; break;
        case IRExpr::Convert: o << "convert." << tn(e.type) << "("; print_expr(o, *e.args[0], fn); o << ")"; break;
        case IRExpr::Intrinsic:
            o << e.op << "(";
            for (size_t i = 0; i < e.args.size(); i++) { if (i) o << ", "; print_expr(o, *e.args[i], fn); }
            o << ")";
            break;
    }
}

void print_block(std::ostream& o, const std::vector<IRStmtP>& body, const IRFunction& fn, int indent) {
    std::string pad(size_t(indent) * 2, ' ');
    for (auto& s : body) {
        switch (s->kind) {
            case IRStmt::SetLocal: {
                IRExpr l; l.kind = IRExpr::Local; l.local = s->local;
                o << pad; print_expr(o, l, fn); o << " = "; print_expr(o, *s->value, fn); o << "\n";
                break;
            }
            case IRStmt::Store: {
                IRExpr l; l.kind = IRExpr::Local; l.local = s->local;
                o << pad << "store." << tn(s->type) << " "; print_expr(o, l, fn); o << "["; print_expr(o, *s->index, fn); o << "] = ";
                print_expr(o, *s->value, fn); o << "\n";
                break;
            }
            case IRStmt::If:
                o << pad << "if "; print_expr(o, *s->cond, fn); o << " {\n";
                print_block(o, s->then_body, fn, indent + 1);
                if (!s->else_body.empty()) { o << pad << "} else {\n"; print_block(o, s->else_body, fn, indent + 1); }
                o << pad << "}\n";
                break;
            case IRStmt::Loop:
                o << pad << "loop while "; print_expr(o, *s->cond, fn); o << " {\n";
                print_block(o, s->body, fn, indent + 1);
                if (!s->step.empty()) { o << pad << "} step {\n"; print_block(o, s->step, fn, indent + 1); }
                o << pad << "}\n";
                break;
            case IRStmt::Return: o << pad << "return\n"; break;
            case IRStmt::Break: o << pad << "break\n"; break;
            case IRStmt::Continue: o << pad << "continue\n"; break;
        }
    }
}

// ---------------------------------------------------------------------------- WASM emission
using wasm::write_sleb;
using wasm::write_uleb;

struct Emitter {
    std::vector<uint8_t> code;
    // Branch depths: how many structured blocks deep we are relative to the innermost loop's
    // break (outer block) and continue (inner block) targets.
    std::vector<uint32_t> break_depth, continue_depth;
    uint32_t depth = 0;

    void op(uint8_t b) { code.push_back(b); }
    void local_get(uint32_t i) { op(0x20); write_uleb(code, i); }
    void local_set(uint32_t i) { op(0x21); write_uleb(code, i); }
    void i32_const(int32_t v) { op(0x41); write_sleb(code, v); }
    void f32_const(uint32_t bits) { op(0x43); for (int i = 0; i < 4; i++) code.push_back(uint8_t(bits >> (8 * i))); }
    void begin(uint8_t kind) { op(kind); op(0x40); depth++; }
    void end() { op(0x0B); depth--; }

    void address(uint32_t base_local, const IRExpr& index) {
        local_get(base_local);
        expr(index);
        i32_const(4);
        op(0x6C);  // i32.mul
        op(0x6A);  // i32.add
    }

    void expr(const IRExpr& e) {
        bool f = e.kind == IRExpr::Binary ? e.args[0]->type == IRType::F32 : e.type == IRType::F32;
        switch (e.kind) {
            case IRExpr::Const: if (e.type == IRType::F32) f32_const(e.bits); else i32_const(int32_t(e.bits)); break;
            case IRExpr::Local: local_get(e.local); break;
            case IRExpr::Load:
                address(e.local, *e.args[0]);
                op(e.type == IRType::F32 ? 0x2A : 0x28); write_uleb(code, 2); write_uleb(code, 0);
                break;
            case IRExpr::Binary: {
                expr(*e.args[0]);
                expr(*e.args[1]);
                const std::string& o = e.op;
                if (o == "&&") { op(0x71); break; }
                if (o == "||") { op(0x72); break; }
                if (f) {
                    if (o == "+") op(0x92); else if (o == "-") op(0x93); else if (o == "*") op(0x94); else if (o == "/") op(0x95);
                    else if (o == "==") op(0x5B); else if (o == "!=") op(0x5C); else if (o == "<") op(0x5D);
                    else if (o == ">") op(0x5E); else if (o == "<=") op(0x5F); else if (o == ">=") op(0x60);
                } else {
                    if (o == "+") op(0x6A); else if (o == "-") op(0x6B); else if (o == "*") op(0x6C); else if (o == "/") op(0x6D);
                    else if (o == "%") op(0x6F); else if (o == "==") op(0x46); else if (o == "!=") op(0x47);
                    else if (o == "<") op(0x48); else if (o == ">") op(0x4A); else if (o == "<=") op(0x4C); else if (o == ">=") op(0x4E);
                }
                break;
            }
            case IRExpr::Unary:
                if (e.op == "!") { expr(*e.args[0]); op(0x45); }
                else if (e.type == IRType::F32) { expr(*e.args[0]); op(0x8C); }
                else { i32_const(0); expr(*e.args[0]); op(0x6B); }
                break;
            case IRExpr::Convert:
                expr(*e.args[0]);
                op(e.type == IRType::F32 ? 0xB2 : 0xA8);  // f32.convert_i32_s / i32.trunc_f32_s
                break;
            case IRExpr::Intrinsic: {
                const std::string& n = e.op;
                if (e.type == IRType::I32 && (n == "min" || n == "max")) {
                    // select(a, b, a < b) for min; a > b for max. Operands are side-effect free.
                    expr(*e.args[0]); expr(*e.args[1]);
                    expr(*e.args[0]); expr(*e.args[1]);
                    op(n == "min" ? 0x48 : 0x4A);
                    op(0x1B);
                    break;
                }
                for (auto& a : e.args) expr(*a);
                if (n == "sqrtf") op(0x91); else if (n == "fabsf") op(0x8B); else if (n == "floorf") op(0x8E);
                else if (n == "ceilf") op(0x8D); else if (n == "fminf" || n == "min") op(0x96); else if (n == "fmaxf" || n == "max") op(0x97);
                break;
            }
        }
    }

    void stmts(const std::vector<IRStmtP>& body) { for (auto& s : body) stmt(*s); }

    void stmt(const IRStmt& s) {
        switch (s.kind) {
            case IRStmt::SetLocal: expr(*s.value); local_set(s.local); break;
            case IRStmt::Store:
                address(s.local, *s.index);
                expr(*s.value);
                op(s.type == IRType::F32 ? 0x38 : 0x36); write_uleb(code, 2); write_uleb(code, 0);
                break;
            case IRStmt::If:
                expr(*s.cond);
                begin(0x04);
                stmts(s.then_body);
                if (!s.else_body.empty()) { op(0x05); stmts(s.else_body); }
                end();
                break;
            case IRStmt::Loop: {
                // block $break { loop $top { br_if $break !cond; block $continue { body } step; br $top } }
                begin(0x02);
                break_depth.push_back(depth);
                begin(0x03);
                uint32_t top = depth;
                expr(*s.cond);
                op(0x45);
                op(0x0D); write_uleb(code, depth - break_depth.back());
                begin(0x02);
                continue_depth.push_back(depth);
                stmts(s.body);
                continue_depth.pop_back();
                end();
                stmts(s.step);
                op(0x0C); write_uleb(code, depth - top);
                end();
                break_depth.pop_back();
                end();
                break;
            }
            case IRStmt::Return: op(0x0F); break;
            case IRStmt::Break: op(0x0C); write_uleb(code, depth - break_depth.back()); break;
            case IRStmt::Continue: op(0x0C); write_uleb(code, depth - continue_depth.back()); break;
        }
    }
};

void section(std::vector<uint8_t>& out, uint8_t id, const std::vector<uint8_t>& body) {
    out.push_back(id);
    write_uleb(out, body.size());
    out.insert(out.end(), body.begin(), body.end());
}
void name(std::vector<uint8_t>& out, const std::string& s) {
    write_uleb(out, s.size());
    out.insert(out.end(), s.begin(), s.end());
}
uint8_t vt(IRType t) { return t == IRType::F32 ? 0x7D : 0x7F; }
}  // namespace

uint32_t fold_constants(IRFunction& fn) {
    uint32_t count = 0;
    fold_block(fn.body, count);
    return count;
}

std::string print_ir(const IRFunction& fn) {
    std::ostringstream o;
    o << "kernel " << fn.name << "(";
    for (size_t i = 4; i < fn.param_types.size(); i++) o << (i > 4 ? ", " : "") << "%p" << (i - 4) << ": " << tn(fn.param_types[i]);
    o << ") {\n";
    for (size_t i = 0; i < fn.local_types.size(); i++) o << "  local %" << fn.local_names[i] << ": " << tn(fn.local_types[i]) << "\n";
    print_block(o, fn.body, fn, 1);
    o << "}\n";
    return o.str();
}

std::vector<uint8_t> emit_wasm(const IRFunction& fn) {
    std::vector<uint8_t> m = {0x00, 'a', 's', 'm', 0x01, 0x00, 0x00, 0x00};
    const size_t user = fn.param_types.size() - 4;

    // Types: 0 = kernel(threadIdx, blockIdx, blockDim, gridDim, params...), 1 = dispatch(grid, block, params...)
    std::vector<uint8_t> types;
    write_uleb(types, 2);
    types.push_back(0x60);
    write_uleb(types, fn.param_types.size());
    for (IRType t : fn.param_types) types.push_back(vt(t));
    write_uleb(types, 0);
    types.push_back(0x60);
    write_uleb(types, 2 + user);
    types.push_back(0x7F); types.push_back(0x7F);
    for (size_t i = 4; i < fn.param_types.size(); i++) types.push_back(vt(fn.param_types[i]));
    write_uleb(types, 0);
    section(m, 1, types);

    // Import the shared linear memory so every kernel sees the same buffers.
    std::vector<uint8_t> imports;
    write_uleb(imports, 1);
    name(imports, "env"); name(imports, "memory");
    imports.push_back(0x02); imports.push_back(0x00); write_uleb(imports, 1);
    section(m, 2, imports);

    std::vector<uint8_t> funcs;
    write_uleb(funcs, 2); write_uleb(funcs, 0); write_uleb(funcs, 1);
    section(m, 3, funcs);

    std::vector<uint8_t> exports;
    write_uleb(exports, 2);
    name(exports, fn.name); exports.push_back(0x00); write_uleb(exports, 0);
    name(exports, fn.name + "__dispatch"); exports.push_back(0x00); write_uleb(exports, 1);
    section(m, 7, exports);

    std::vector<uint8_t> code;
    write_uleb(code, 2);
    {   // kernel body
        Emitter e;
        std::vector<uint8_t> body;
        write_uleb(body, fn.local_types.size());
        for (IRType t : fn.local_types) { write_uleb(body, 1); body.push_back(vt(t)); }
        e.stmts(fn.body);
        e.op(0x0B);
        body.insert(body.end(), e.code.begin(), e.code.end());
        write_uleb(code, body.size());
        code.insert(code.end(), body.begin(), body.end());
    }
    {   // dispatch: for b in [0, grid) for t in [0, block) kernel(t, b, block, grid, params...)
        Emitter e;
        const uint32_t grid = 0, block = 1, b = uint32_t(2 + user), t = b + 1;
        std::vector<uint8_t> body;
        write_uleb(body, 1); write_uleb(body, 2); body.push_back(0x7F);
        e.i32_const(0); e.local_set(b);
        e.begin(0x02); e.begin(0x03);
        e.local_get(b); e.local_get(grid); e.op(0x4F); e.op(0x0D); write_uleb(e.code, 1);   // br_if b >= grid (unsigned)
        e.i32_const(0); e.local_set(t);
        e.begin(0x02); e.begin(0x03);
        e.local_get(t); e.local_get(block); e.op(0x4F); e.op(0x0D); write_uleb(e.code, 1);
        e.local_get(t); e.local_get(b); e.local_get(block); e.local_get(grid);
        for (uint32_t i = 0; i < user; i++) e.local_get(2 + i);
        e.op(0x10); write_uleb(e.code, 0);
        e.local_get(t); e.i32_const(1); e.op(0x6A); e.local_set(t);
        e.op(0x0C); write_uleb(e.code, 0);
        e.end(); e.end();
        e.local_get(b); e.i32_const(1); e.op(0x6A); e.local_set(b);
        e.op(0x0C); write_uleb(e.code, 0);
        e.end(); e.end();
        e.op(0x0B);
        body.insert(body.end(), e.code.begin(), e.code.end());
        write_uleb(code, body.size());
        code.insert(code.end(), body.begin(), body.end());
    }
    section(m, 10, code);
    return m;
}

CompiledKernel compile(const std::string& source) {
    CompiledKernel out;
    KernelAST ast;
    if (!parse(source, ast, out.error)) return out;
    IRFunction fn;
    if (!lower(ast, fn, out.error)) return out;
    out.folded_constants = fold_constants(fn);
    out.name = fn.name;
    for (auto& p : ast.params) out.params.push_back({p.type, p.name});
    out.ir_text = print_ir(fn);
    out.wasm = emit_wasm(fn);
    out.ok = true;
    return out;
}

}  // namespace unify::kernel
