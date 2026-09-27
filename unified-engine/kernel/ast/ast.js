/**
 * Kernel AST. Deliberately small: expressions, statements, and a function
 * signature with typed parameters. No types are inferred here.
 *
 *   TypeRef  { kind: 'scalar' | 'buffer', scalar, isConst }
 *   Expr    { kind, ... }  kinds: int float bool var builtin index unary
 *                         binary assign call cast
 *                         (float carries `single`: true for an `f` suffix)
 *   Stmt    { kind, ... }  kinds: decl assign if while for break continue
 *                         return expr block
 *   Param   { name, type }
 *   KernelAst { name, returnType, params, body, isGlobal }
 *
 * The shapes are documented rather than declared as types because the project
 * runs as plain ESM JavaScript with no TypeScript toolchain.
 */
export {};
