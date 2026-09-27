#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace unify::kernel {

// ============================================================================ AST
enum class Type : uint8_t { Void, Int, Float, IntPtr, FloatPtr };
const char* type_name(Type t);

struct Expr;
using ExprP = std::unique_ptr<Expr>;
struct Expr {
    enum Kind { IntLit, FloatLit, Var, Index, Binary, Unary, Cast, Call } kind;
    int line = 0;
    int32_t ival = 0;
    float fval = 0;
    std::string name;        // Var / Call name / operator spelling
    Type cast_to = Type::Void;
    std::vector<ExprP> args; // operands / call arguments / [base, index]
};

struct Stmt;
using StmtP = std::unique_ptr<Stmt>;
struct Stmt {
    enum Kind { Decl, Assign, If, For, While, Block, Return, Break, Continue } kind;
    int line = 0;
    Type decl_type = Type::Void;
    std::string name;        // Decl name; Assign operator ("=", "+=", ...)
    ExprP target, value, cond;
    StmtP init, step, then_s, else_s, body;
    std::vector<StmtP> stmts;
};

struct Param { Type type; std::string name; };
struct KernelAST { std::string name; std::vector<Param> params; StmtP body; };

// ============================================================================ IR
// Typed, name-resolved, structured IR. Every value is i32 or f32; pointers are i32 byte
// offsets into the shared linear memory. Implicit conversions are explicit nodes here.
enum class IRType : uint8_t { I32, F32 };

struct IRExpr;
using IRExprP = std::unique_ptr<IRExpr>;
struct IRExpr {
    enum Kind { Const, Local, Load, Binary, Unary, Convert, Intrinsic } kind;
    IRType type = IRType::I32;
    uint32_t bits = 0;       // Const payload (i32 value or f32 bits)
    uint32_t local = 0;      // Local index / Load base pointer local
    std::string op;          // Binary/Unary/Intrinsic operator
    std::vector<IRExprP> args;
};

struct IRStmt;
using IRStmtP = std::unique_ptr<IRStmt>;
struct IRStmt {
    enum Kind { SetLocal, Store, If, Loop, Return, Break, Continue } kind;
    uint32_t local = 0;      // SetLocal target / Store base pointer local
    IRType type = IRType::I32;
    IRExprP value, index, cond;
    std::vector<IRStmtP> then_body, else_body, body, step;
};

struct IRFunction {
    std::string name;
    std::vector<IRType> param_types;  // builtins (4 x i32) followed by user params
    std::vector<IRType> local_types;  // declared locals after params
    std::vector<std::string> local_names;
    std::vector<IRStmtP> body;
};

// ============================================================================ API
struct KernelParam { Type type; std::string name; };

struct CompiledKernel {
    bool ok = false;
    std::string error;               // "line N: message" on failure
    std::string name;
    std::vector<KernelParam> params; // user-visible parameters (excluding builtins)
    std::vector<uint8_t> wasm;       // binary module: imports env.memory, exports name and name__dispatch
    std::string ir_text;             // human-readable IR, for inspection and tests
    uint32_t folded_constants = 0;   // optimisation statistic
};

/// Full pipeline: source → AST → IR → optimise → WASM.
CompiledKernel compile(const std::string& source);

// Individual stages (exposed for tests and tooling).
bool parse(const std::string& source, KernelAST& out, std::string& error);
bool lower(const KernelAST& ast, IRFunction& out, std::string& error);
uint32_t fold_constants(IRFunction& fn);
std::string print_ir(const IRFunction& fn);
std::vector<uint8_t> emit_wasm(const IRFunction& fn);

}  // namespace unify::kernel
