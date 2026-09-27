// AST → IR: name resolution, type checking, implicit conversions, control-flow lowering.
#include "kernel.h"
#include <cstring>
#include <map>

namespace unify::kernel {

namespace {
const char* kBuiltins[] = {"threadIdx.x", "blockIdx.x", "blockDim.x", "gridDim.x"};

uint32_t fbits(float f) { uint32_t b; std::memcpy(&b, &f, 4); return b; }

class Lowerer {
public:
    std::string err;

    bool run(const KernelAST& ast, IRFunction& fn) {
        fn_ = &fn;
        fn.name = ast.name;
        scopes_.emplace_back();
        for (const char* b : kBuiltins) { declare_param(b, Type::Int); }
        for (auto& p : ast.params) {
            if (p.type == Type::Void) return fail(0, "parameter '" + p.name + "' cannot be void");
            if (lookup(p.name)) return fail(0, "duplicate parameter '" + p.name + "'");
            declare_param(p.name, p.type);
        }
        return stmt(*ast.body, fn.body);
    }

private:
    struct Sym { uint32_t local; Type type; };

    bool fail(int line, const std::string& m) { if (err.empty()) err = "line " + std::to_string(line) + ": " + m; return false; }
    static IRType ir(Type t) { return t == Type::Float ? IRType::F32 : IRType::I32; }

    void declare_param(const std::string& name, Type t) {
        uint32_t idx = uint32_t(fn_->param_types.size());
        fn_->param_types.push_back(ir(t));
        scopes_.back()[name] = {idx, t};
        param_count_++;
    }
    uint32_t declare_local(const std::string& name, Type t) {
        uint32_t idx = param_count_ + uint32_t(fn_->local_types.size());
        fn_->local_types.push_back(ir(t));
        fn_->local_names.push_back(name);
        scopes_.back()[name] = {idx, t};
        return idx;
    }
    const Sym* lookup(const std::string& name) const {
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
            auto f = it->find(name);
            if (f != it->end()) return &f->second;
        }
        return nullptr;
    }

    static IRExprP convert(IRExprP e, IRType to) {
        if (e->type == to) return e;
        auto c = std::make_unique<IRExpr>();
        c->kind = IRExpr::Convert;
        c->type = to;
        c->args.push_back(std::move(e));
        return c;
    }
    static IRExprP konst_i(int32_t v) { auto e = std::make_unique<IRExpr>(); e->kind = IRExpr::Const; e->type = IRType::I32; e->bits = uint32_t(v); return e; }
    // Normalises a scalar to an i32 truth value (0/1).
    static IRExprP truthy(IRExprP e) {
        auto c = std::make_unique<IRExpr>();
        c->kind = IRExpr::Binary; c->type = IRType::I32; c->op = "!=";
        IRExprP zero = std::make_unique<IRExpr>();
        zero->kind = IRExpr::Const; zero->type = e->type; zero->bits = 0;
        c->args.push_back(std::move(e));
        c->args.push_back(std::move(zero));
        return c;
    }

    // Returns the lowered expression and its source-level type.
    IRExprP expr(const Expr& e, Type& t) {
        switch (e.kind) {
            case Expr::IntLit: t = Type::Int; return konst_i(e.ival);
            case Expr::FloatLit: {
                t = Type::Float;
                auto c = std::make_unique<IRExpr>();
                c->kind = IRExpr::Const; c->type = IRType::F32; c->bits = fbits(e.fval);
                return c;
            }
            case Expr::Var: {
                const Sym* s = lookup(e.name);
                if (!s) { fail(e.line, "undeclared identifier '" + e.name + "'"); return nullptr; }
                t = s->type;
                auto l = std::make_unique<IRExpr>();
                l->kind = IRExpr::Local; l->type = ir(s->type); l->local = s->local;
                return l;
            }
            case Expr::Index: {
                const Expr& base = *e.args[0];
                if (base.kind != Expr::Var) { fail(e.line, "only named pointers can be indexed"); return nullptr; }
                const Sym* s = lookup(base.name);
                if (!s) { fail(e.line, "undeclared identifier '" + base.name + "'"); return nullptr; }
                if (s->type != Type::IntPtr && s->type != Type::FloatPtr) { fail(e.line, "'" + base.name + "' is not a pointer"); return nullptr; }
                Type it;
                IRExprP idx = expr(*e.args[1], it);
                if (!idx) return nullptr;
                if (it != Type::Int) { fail(e.line, "array index must be int"); return nullptr; }
                t = s->type == Type::FloatPtr ? Type::Float : Type::Int;
                auto l = std::make_unique<IRExpr>();
                l->kind = IRExpr::Load; l->type = ir(t); l->local = s->local;
                l->args.push_back(std::move(idx));
                return l;
            }
            case Expr::Cast: {
                Type it;
                IRExprP x = expr(*e.args[0], it);
                if (!x) return nullptr;
                if (it == Type::IntPtr || it == Type::FloatPtr) { fail(e.line, "cannot cast a pointer"); return nullptr; }
                t = e.cast_to;
                return convert(std::move(x), ir(t));
            }
            case Expr::Unary: {
                Type it;
                IRExprP x = expr(*e.args[0], it);
                if (!x) return nullptr;
                if (it == Type::IntPtr || it == Type::FloatPtr) { fail(e.line, "invalid operand to unary " + e.name); return nullptr; }
                auto u = std::make_unique<IRExpr>();
                u->kind = IRExpr::Unary; u->op = e.name;
                if (e.name == "!") { t = Type::Int; u->type = IRType::I32; u->args.push_back(truthy(std::move(x))); }
                else { t = it; u->type = x->type; u->args.push_back(std::move(x)); }
                return u;
            }
            case Expr::Binary: {
                Type lt, rt;
                IRExprP a = expr(*e.args[0], lt);
                IRExprP b = a ? expr(*e.args[1], rt) : nullptr;
                if (!a || !b) return nullptr;
                if (lt == Type::IntPtr || lt == Type::FloatPtr || rt == Type::IntPtr || rt == Type::FloatPtr) {
                    fail(e.line, "pointer arithmetic is not supported; index the pointer instead");
                    return nullptr;
                }
                const std::string& op = e.name;
                auto bin = std::make_unique<IRExpr>();
                bin->kind = IRExpr::Binary; bin->op = op;
                if (op == "&&" || op == "||") {
                    t = Type::Int; bin->type = IRType::I32;
                    bin->args.push_back(truthy(std::move(a)));
                    bin->args.push_back(truthy(std::move(b)));
                    return bin;
                }
                // Usual arithmetic conversion: int op float -> float.
                IRType operand = (lt == Type::Float || rt == Type::Float) ? IRType::F32 : IRType::I32;
                if (op == "%" && operand == IRType::F32) { fail(e.line, "'%' requires int operands"); return nullptr; }
                bin->args.push_back(convert(std::move(a), operand));
                bin->args.push_back(convert(std::move(b), operand));
                bool cmp = op == "<" || op == "<=" || op == ">" || op == ">=" || op == "==" || op == "!=";
                bin->type = cmp ? IRType::I32 : operand;
                t = cmp ? Type::Int : (operand == IRType::F32 ? Type::Float : Type::Int);
                return bin;
            }
            case Expr::Call: {
                static const std::map<std::string, int> intrinsics = {
                    {"sqrtf", 1}, {"fabsf", 1}, {"floorf", 1}, {"ceilf", 1}, {"fminf", 2}, {"fmaxf", 2}, {"min", 2}, {"max", 2}};
                auto it = intrinsics.find(e.name);
                if (it == intrinsics.end()) { fail(e.line, "unknown function '" + e.name + "'"); return nullptr; }
                if (int(e.args.size()) != it->second) { fail(e.line, "'" + e.name + "' takes " + std::to_string(it->second) + " argument(s)"); return nullptr; }
                auto call = std::make_unique<IRExpr>();
                call->kind = IRExpr::Intrinsic; call->op = e.name;
                bool integer = e.name == "min" || e.name == "max";
                IRType want = IRType::F32;
                if (integer) {
                    // min/max follow their operands' type.
                    want = IRType::I32;
                    for (auto& a : e.args) { Type at; IRExprP x = expr(*a, at); if (!x) return nullptr; if (at == Type::Float) want = IRType::F32; }
                }
                for (auto& a : e.args) {
                    Type at;
                    IRExprP x = expr(*a, at);
                    if (!x) return nullptr;
                    call->args.push_back(convert(std::move(x), want));
                }
                call->type = want;
                t = want == IRType::F32 ? Type::Float : Type::Int;
                return call;
            }
        }
        return nullptr;
    }

    bool stmt(const Stmt& s, std::vector<IRStmtP>& out) {
        switch (s.kind) {
            case Stmt::Block: {
                scopes_.emplace_back();
                for (auto& x : s.stmts) if (!stmt(*x, out)) return false;
                scopes_.pop_back();
                return true;
            }
            case Stmt::Decl: {
                if (s.decl_type == Type::IntPtr || s.decl_type == Type::FloatPtr) return fail(s.line, "local pointers are not supported");
                if (scopes_.back().count(s.name)) return fail(s.line, "redeclaration of '" + s.name + "'");
                IRExprP init;
                if (s.value) {
                    Type vt;
                    init = expr(*s.value, vt);
                    if (!init) return false;
                    init = convert(std::move(init), ir(s.decl_type));
                } else {
                    init = konst_i(0);
                    init->type = ir(s.decl_type);  // zero-initialise: 0 bits are 0 for both i32 and f32
                }
                uint32_t idx = declare_local(s.name, s.decl_type);
                auto st = std::make_unique<IRStmt>();
                st->kind = IRStmt::SetLocal; st->local = idx; st->type = ir(s.decl_type); st->value = std::move(init);
                out.push_back(std::move(st));
                return true;
            }
            case Stmt::Assign: {
                Type vt;
                IRExprP value = expr(*s.value, vt);
                if (!value) return false;
                const Expr& tgt = *s.target;
                std::string op = s.name;
                if (tgt.kind == Expr::Var) {
                    const Sym* sym = lookup(tgt.name);
                    if (!sym) return fail(s.line, "undeclared identifier '" + tgt.name + "'");
                    if (sym->local < 4) return fail(s.line, "cannot assign to builtin '" + tgt.name + "'");
                    if (sym->type == Type::IntPtr || sym->type == Type::FloatPtr) return fail(s.line, "cannot reassign a pointer parameter");
                    IRType ty = ir(sym->type);
                    if (op != "=") {
                        Type dummy;
                        IRExprP cur = expr(tgt, dummy);
                        auto bin = std::make_unique<IRExpr>();
                        bin->kind = IRExpr::Binary; bin->op = op.substr(0, 1);
                        IRType operand = (ty == IRType::F32 || vt == Type::Float) ? IRType::F32 : IRType::I32;
                        bin->type = operand;
                        bin->args.push_back(convert(std::move(cur), operand));
                        bin->args.push_back(convert(std::move(value), operand));
                        value = std::move(bin);
                    }
                    auto st = std::make_unique<IRStmt>();
                    st->kind = IRStmt::SetLocal; st->local = sym->local; st->type = ty; st->value = convert(std::move(value), ty);
                    out.push_back(std::move(st));
                    return true;
                }
                if (tgt.kind == Expr::Index) {
                    Type et;
                    IRExprP load = expr(tgt, et);  // validates pointer + index
                    if (!load) return false;
                    IRType ty = load->type;
                    uint32_t base = load->local;
                    if (op != "=") {
                        auto bin = std::make_unique<IRExpr>();
                        bin->kind = IRExpr::Binary; bin->op = op.substr(0, 1);
                        IRType operand = (ty == IRType::F32 || vt == Type::Float) ? IRType::F32 : IRType::I32;
                        bin->type = operand;
                        Type dummy;
                        IRExprP cur = expr(tgt, dummy);
                        bin->args.push_back(convert(std::move(cur), operand));
                        bin->args.push_back(convert(std::move(value), operand));
                        value = std::move(bin);
                    }
                    auto st = std::make_unique<IRStmt>();
                    st->kind = IRStmt::Store; st->local = base; st->type = ty;
                    st->index = std::move(load->args[0]);
                    st->value = convert(std::move(value), ty);
                    out.push_back(std::move(st));
                    return true;
                }
                return fail(s.line, "left side of assignment is not assignable");
            }
            case Stmt::If: {
                Type ct;
                IRExprP c = expr(*s.cond, ct);
                if (!c) return false;
                auto st = std::make_unique<IRStmt>();
                st->kind = IRStmt::If;
                st->cond = c->type == IRType::I32 ? std::move(c) : truthy(std::move(c));
                scopes_.emplace_back();
                if (!stmt(*s.then_s, st->then_body)) return false;
                scopes_.pop_back();
                if (s.else_s) {
                    scopes_.emplace_back();
                    if (!stmt(*s.else_s, st->else_body)) return false;
                    scopes_.pop_back();
                }
                out.push_back(std::move(st));
                return true;
            }
            case Stmt::For:
            case Stmt::While: {
                scopes_.emplace_back();
                if (s.init && !stmt(*s.init, out)) return false;
                auto st = std::make_unique<IRStmt>();
                st->kind = IRStmt::Loop;
                if (s.cond) {
                    Type ct;
                    IRExprP c = expr(*s.cond, ct);
                    if (!c) return false;
                    st->cond = c->type == IRType::I32 ? std::move(c) : truthy(std::move(c));
                } else {
                    st->cond = konst_i(1);
                }
                loop_depth_++;
                if (!stmt(*s.body, st->body)) return false;
                loop_depth_--;
                if (s.step && !stmt(*s.step, st->step)) return false;
                scopes_.pop_back();
                out.push_back(std::move(st));
                return true;
            }
            case Stmt::Return: { auto st = std::make_unique<IRStmt>(); st->kind = IRStmt::Return; out.push_back(std::move(st)); return true; }
            case Stmt::Break:
            case Stmt::Continue: {
                if (loop_depth_ == 0) return fail(s.line, std::string(s.kind == Stmt::Break ? "break" : "continue") + " outside a loop");
                auto st = std::make_unique<IRStmt>();
                st->kind = s.kind == Stmt::Break ? IRStmt::Break : IRStmt::Continue;
                out.push_back(std::move(st));
                return true;
            }
        }
        return fail(s.line, "unsupported statement");
    }

    IRFunction* fn_ = nullptr;
    std::vector<std::map<std::string, Sym>> scopes_;
    uint32_t param_count_ = 0;
    int loop_depth_ = 0;
};
}  // namespace

bool lower(const KernelAST& ast, IRFunction& out, std::string& error) {
    out = IRFunction{};
    Lowerer l;
    if (!l.run(ast, out)) { error = l.err; return false; }
    return true;
}

}  // namespace unify::kernel
