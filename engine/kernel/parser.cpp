// Kernel language front end: lexer + recursive-descent parser → AST.
//
//   __global__ void vadd(const float* A, const float* B, float* C, int n) {
//       int i = blockIdx.x * blockDim.x + threadIdx.x;
//       if (i < n) C[i] = A[i] + B[i];
//   }
#include "kernel.h"
#include <cctype>
#include <cstdlib>

namespace unify::kernel {

const char* type_name(Type t) {
    switch (t) {
        case Type::Void: return "void";
        case Type::Int: return "int";
        case Type::Float: return "float";
        case Type::IntPtr: return "int*";
        case Type::FloatPtr: return "float*";
    }
    return "?";
}

namespace {
struct Token {
    enum Kind { Ident, Int, Float, Punct, End } kind;
    std::string text;
    int line;
};

bool lex(const std::string& s, std::vector<Token>& out, std::string& err) {
    int line = 1;
    size_t i = 0;
    static const char* two[] = {"<=", ">=", "==", "!=", "&&", "||", "+=", "-=", "*=", "/=", "++", "--"};
    while (i < s.size()) {
        char c = s[i];
        if (c == '\n') { line++; i++; continue; }
        if (std::isspace(uint8_t(c))) { i++; continue; }
        if (c == '/' && i + 1 < s.size() && s[i + 1] == '/') { while (i < s.size() && s[i] != '\n') i++; continue; }
        if (c == '/' && i + 1 < s.size() && s[i + 1] == '*') {
            i += 2;
            while (i + 1 < s.size() && !(s[i] == '*' && s[i + 1] == '/')) { if (s[i] == '\n') line++; i++; }
            i += 2;
            continue;
        }
        if (std::isalpha(uint8_t(c)) || c == '_') {
            size_t j = i;
            while (j < s.size() && (std::isalnum(uint8_t(s[j])) || s[j] == '_')) j++;
            out.push_back({Token::Ident, s.substr(i, j - i), line});
            i = j;
            continue;
        }
        if (std::isdigit(uint8_t(c)) || (c == '.' && i + 1 < s.size() && std::isdigit(uint8_t(s[i + 1])))) {
            size_t j = i;
            bool is_float = false;
            while (j < s.size() && (std::isdigit(uint8_t(s[j])) || s[j] == '.' || s[j] == 'e' || s[j] == 'E' ||
                                    ((s[j] == '-' || s[j] == '+') && (s[j - 1] == 'e' || s[j - 1] == 'E')))) {
                if (s[j] == '.' || s[j] == 'e' || s[j] == 'E') is_float = true;
                j++;
            }
            std::string num = s.substr(i, j - i);
            if (j < s.size() && (s[j] == 'f' || s[j] == 'F')) { is_float = true; j++; }
            out.push_back({is_float ? Token::Float : Token::Int, num, line});
            i = j;
            continue;
        }
        bool matched = false;
        for (const char* t : two)
            if (s.compare(i, 2, t) == 0) { out.push_back({Token::Punct, t, line}); i += 2; matched = true; break; }
        if (matched) continue;
        if (std::string("+-*/%<>=!(){}[];,.&").find(c) != std::string::npos) {
            out.push_back({Token::Punct, std::string(1, c), line});
            i++;
            continue;
        }
        err = "line " + std::to_string(line) + ": unexpected character '" + std::string(1, c) + "'";
        return false;
    }
    out.push_back({Token::End, "", line});
    return true;
}

class Parser {
public:
    explicit Parser(std::vector<Token> t) : t_(std::move(t)) {}
    std::string err;

    bool kernel(KernelAST& k) {
        while (peek("__global__") || peek("extern") || peek("\"C\"")) next();
        if (!expect("void")) return false;
        if (!ident(k.name)) return false;
        if (!expect("(")) return false;
        if (!peek(")")) {
            do {
                Param p;
                if (!type(p.type)) return false;
                if (!ident(p.name)) return false;
                k.params.push_back(p);
            } while (accept(","));
        }
        if (!expect(")")) return false;
        k.body = block();
        if (!k.body) return false;
        if (t_[pos_].kind != Token::End) return fail("unexpected tokens after kernel body");
        return true;
    }

private:
    const Token& cur() const { return t_[pos_]; }
    void next() { if (t_[pos_].kind != Token::End) pos_++; }
    bool peek(const char* s) const { return cur().text == s && cur().kind != Token::End; }
    bool accept(const char* s) { if (peek(s)) { next(); return true; } return false; }
    bool fail(const std::string& m) { if (err.empty()) err = "line " + std::to_string(cur().line) + ": " + m; return false; }
    bool expect(const char* s) { if (accept(s)) return true; return fail(std::string("expected '") + s + "' but found '" + cur().text + "'"); }
    bool ident(std::string& out) {
        if (cur().kind != Token::Ident) return fail("expected identifier but found '" + cur().text + "'");
        out = cur().text;
        next();
        return true;
    }
    bool is_type_start() const { return peek("int") || peek("float") || peek("const") || peek("unsigned"); }
    bool type(Type& t) {
        accept("const");
        if (accept("int")) t = Type::Int;
        else if (accept("float")) t = Type::Float;
        else return fail("expected a type (int or float) but found '" + cur().text + "'");
        if (accept("*")) t = t == Type::Int ? Type::IntPtr : Type::FloatPtr;
        accept("const");
        accept("__restrict__");
        return true;
    }

    StmtP block() {
        auto s = std::make_unique<Stmt>();
        s->kind = Stmt::Block;
        s->line = cur().line;
        if (!expect("{")) return nullptr;
        while (!peek("}")) {
            if (cur().kind == Token::End) { fail("unterminated block"); return nullptr; }
            StmtP st = statement();
            if (!st) return nullptr;
            s->stmts.push_back(std::move(st));
        }
        next();
        return s;
    }

    StmtP simple_statement() {  // declaration or assignment, no trailing ';'
        auto s = std::make_unique<Stmt>();
        s->line = cur().line;
        if (is_type_start()) {
            s->kind = Stmt::Decl;
            if (!type(s->decl_type) || !ident(s->name)) return nullptr;
            if (accept("=")) { s->value = expr(); if (!s->value) return nullptr; }
            return s;
        }
        s->kind = Stmt::Assign;
        s->target = postfix();
        if (!s->target) return nullptr;
        if (accept("++") || accept("--")) {
            std::string op = t_[pos_ - 1].text == "++" ? "+=" : "-=";
            s->name = op;
            auto one = std::make_unique<Expr>();
            one->kind = Expr::IntLit; one->ival = 1; one->line = s->line;
            s->value = std::move(one);
            return s;
        }
        for (const char* op : {"=", "+=", "-=", "*=", "/="})
            if (accept(op)) { s->name = op; s->value = expr(); return s->value ? std::move(s) : nullptr; }
        fail("expected an assignment");
        return nullptr;
    }

    StmtP statement() {
        int line = cur().line;
        if (peek("{")) return block();
        if (accept("if")) {
            auto s = std::make_unique<Stmt>();
            s->kind = Stmt::If; s->line = line;
            if (!expect("(")) return nullptr;
            s->cond = expr();
            if (!s->cond || !expect(")")) return nullptr;
            s->then_s = statement();
            if (!s->then_s) return nullptr;
            if (accept("else")) { s->else_s = statement(); if (!s->else_s) return nullptr; }
            return s;
        }
        if (accept("for")) {
            auto s = std::make_unique<Stmt>();
            s->kind = Stmt::For; s->line = line;
            if (!expect("(")) return nullptr;
            if (!peek(";")) { s->init = simple_statement(); if (!s->init) return nullptr; }
            if (!expect(";")) return nullptr;
            if (!peek(";")) { s->cond = expr(); if (!s->cond) return nullptr; }
            if (!expect(";")) return nullptr;
            if (!peek(")")) { s->step = simple_statement(); if (!s->step) return nullptr; }
            if (!expect(")")) return nullptr;
            s->body = statement();
            return s->body ? std::move(s) : nullptr;
        }
        if (accept("while")) {
            auto s = std::make_unique<Stmt>();
            s->kind = Stmt::While; s->line = line;
            if (!expect("(")) return nullptr;
            s->cond = expr();
            if (!s->cond || !expect(")")) return nullptr;
            s->body = statement();
            return s->body ? std::move(s) : nullptr;
        }
        for (auto [kw, kind] : {std::pair<const char*, Stmt::Kind>{"return", Stmt::Return}, {"break", Stmt::Break}, {"continue", Stmt::Continue}}) {
            if (accept(kw)) {
                auto s = std::make_unique<Stmt>();
                s->kind = kind; s->line = line;
                return expect(";") ? std::move(s) : nullptr;
            }
        }
        StmtP s = simple_statement();
        if (!s || !expect(";")) return nullptr;
        return s;
    }

    // Precedence climbing: || && ==,!= <,<=,>,>= +,- *,/,%
    ExprP expr() { return binary(0); }
    int prec(const std::string& op) const {
        if (op == "||") return 1;
        if (op == "&&") return 2;
        if (op == "==" || op == "!=") return 3;
        if (op == "<" || op == "<=" || op == ">" || op == ">=") return 4;
        if (op == "+" || op == "-") return 5;
        if (op == "*" || op == "/" || op == "%") return 6;
        return -1;
    }
    ExprP binary(int min_prec) {
        ExprP lhs = unary();
        if (!lhs) return nullptr;
        while (cur().kind == Token::Punct && prec(cur().text) > min_prec) {
            std::string op = cur().text;
            int line = cur().line;
            next();
            ExprP rhs = binary(prec(op));
            if (!rhs) return nullptr;
            auto e = std::make_unique<Expr>();
            e->kind = Expr::Binary; e->name = op; e->line = line;
            e->args.push_back(std::move(lhs));
            e->args.push_back(std::move(rhs));
            lhs = std::move(e);
        }
        return lhs;
    }
    ExprP unary() {
        int line = cur().line;
        if (accept("-") || accept("!")) {
            auto e = std::make_unique<Expr>();
            e->kind = Expr::Unary; e->name = t_[pos_ - 1].text; e->line = line;
            ExprP x = unary();
            if (!x) return nullptr;
            e->args.push_back(std::move(x));
            return e;
        }
        // Cast: "(" type ")" unary
        if (peek("(") && (t_[pos_ + 1].text == "int" || t_[pos_ + 1].text == "float")) {
            next();
            Type t;
            if (!type(t) || !expect(")")) return nullptr;
            auto e = std::make_unique<Expr>();
            e->kind = Expr::Cast; e->cast_to = t; e->line = line;
            ExprP x = unary();
            if (!x) return nullptr;
            e->args.push_back(std::move(x));
            return e;
        }
        return postfix();
    }
    ExprP postfix() {
        ExprP e = primary();
        if (!e) return nullptr;
        while (true) {
            int line = cur().line;
            if (accept("[")) {
                auto ix = std::make_unique<Expr>();
                ix->kind = Expr::Index; ix->line = line;
                ix->args.push_back(std::move(e));
                ExprP idx = expr();
                if (!idx || !expect("]")) return nullptr;
                ix->args.push_back(std::move(idx));
                e = std::move(ix);
            } else if (accept(".")) {
                // Only builtin member access (threadIdx.x etc.) exists in this language.
                std::string field;
                if (!ident(field)) return nullptr;
                if (e->kind != Expr::Var) { fail("member access is only valid on builtins"); return nullptr; }
                e->name += "." + field;
            } else {
                return e;
            }
        }
    }
    ExprP primary() {
        auto e = std::make_unique<Expr>();
        e->line = cur().line;
        if (cur().kind == Token::Int) { e->kind = Expr::IntLit; e->ival = int32_t(std::strtol(cur().text.c_str(), nullptr, 0)); next(); return e; }
        if (cur().kind == Token::Float) { e->kind = Expr::FloatLit; e->fval = std::strtof(cur().text.c_str(), nullptr); next(); return e; }
        if (cur().kind == Token::Ident) {
            e->name = cur().text;
            next();
            if (accept("(")) {
                e->kind = Expr::Call;
                if (!peek(")")) {
                    do { ExprP a = expr(); if (!a) return nullptr; e->args.push_back(std::move(a)); } while (accept(","));
                }
                if (!expect(")")) return nullptr;
                return e;
            }
            e->kind = Expr::Var;
            return e;
        }
        if (accept("(")) {
            ExprP inner = expr();
            if (!inner || !expect(")")) return nullptr;
            return inner;
        }
        fail("expected an expression but found '" + cur().text + "'");
        return nullptr;
    }

    std::vector<Token> t_;
    size_t pos_ = 0;
};
}  // namespace

bool parse(const std::string& source, KernelAST& out, std::string& error) {
    std::vector<Token> tokens;
    if (!lex(source, tokens, error)) return false;
    Parser p(std::move(tokens));
    if (!p.kernel(out)) { error = p.err; return false; }
    return true;
}

}  // namespace unify::kernel
