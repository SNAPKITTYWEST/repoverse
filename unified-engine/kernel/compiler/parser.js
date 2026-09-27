/**
 * Recursive-descent parser for the CUDA-like kernel language.
 * Produces the AST in ../ast/ast.js. Every failure raises ParseError with a line.
 */

import { tokenize, TokenType } from './lexer.js';

export class ParseError extends Error {
  constructor(message, line) {
    super(`${message} (line ${line})`);
    this.name = 'ParseError';
    this.line = line;
  }
}

const SCALARS = new Set(['int', 'uint', 'float', 'double', 'void']);
const BUILTINS = new Set([
  'blockIdx', 'blockDim', 'threadIdx', 'gridDim',
  'global_id', 'local_id', 'block_id', 'global_id_x', 'local_id_x',
]);

/** Binary operator precedence, higher binds tighter. Follows C. */
const PRECEDENCE = {
  '||': 1, '&&': 2,
  '|': 3, '^': 4, '&': 5,
  '==': 6, '!=': 6,
  '<': 7, '>': 7, '<=': 7, '>=': 7,
  '<<': 8, '>>': 8,
  '+': 9, '-': 9,
  '*': 10, '/': 10, '%': 10,
};

const ASSIGN_OPS = new Set(['=', '+=', '-=', '*=', '/=', '%=']);

/** Maps CUDA-style builtins onto the backend-neutral IR names. */
const BUILTIN_MEMBERS = {
  blockIdx: { x: 'block_id_x', y: 'block_id_y', z: 'block_id_z' },
  blockDim: { x: 'block_dim_x', y: 'block_dim_y', z: 'block_dim_z' },
  threadIdx: { x: 'local_id_x', y: 'local_id_y', z: 'local_id_z' },
  gridDim: { x: 'grid_dim_x', y: 'grid_dim_y', z: 'grid_dim_z' },
};

export function parse(source) {
  const tokens = tokenize(source);
  let pos = 0;

  const peek = (offset = 0) => tokens[Math.min(pos + offset, tokens.length - 1)];
  const at = (value, type = TokenType.PUNCT) => peek().type === type && peek().value === value;
  const atKeyword = (value) => peek().type === TokenType.KEYWORD && peek().value === value;
  const next = () => tokens[pos++];
  const expect = (value) => {
    if (!at(value)) throw new ParseError(`Expected '${value}' but found '${peek().value}'`, peek().line);
    return next();
  };
  const atScalar = () => peek().type === TokenType.KEYWORD && SCALARS.has(peek().value) && peek().value !== 'void';
  const expectIdent = () => {
    if (peek().type !== TokenType.IDENT) throw new ParseError(`Expected an identifier but found '${peek().value}'`, peek().line);
    return next().value;
  };

  // ---- parameter type: `const float* A` or `int N` ----
  function parseParamType() {
    const isConst = atKeyword('const');
    if (isConst) next();
    const typeToken = next();
    if (typeToken.type !== TokenType.KEYWORD || !SCALARS.has(typeToken.value)) {
      throw new ParseError(`'${typeToken.value}' is not a supported parameter type`, typeToken.line);
    }
    const scalar = typeToken.value;
    let isBuffer = false;
    if (at('*')) {
      next();
      isBuffer = true;
    }
    return { kind: isBuffer ? 'buffer' : 'scalar', scalar, isConst };
  }

  function parseParams() {
    expect('(');
    const params = [];
    if (at(')')) {
      next();
      return params;
    }
    for (;;) {
      const type = parseParamType();
      const name = expectIdent();
      params.push({ name, type });
      if (at(',')) {
        next();
        if (at(')')) break;
        continue;
      }
      break;
    }
    expect(')');
    return params;
  }

  // ---- primary expressions ----
  function parsePrimary() {
    const token = peek();

    if (at('(')) {
      next();
      const inner = parseExpr();
      expect(')');
      return inner;
    }
    if (token.type === TokenType.INT) {
      next();
      const value = token.value.startsWith('0x') || token.value.startsWith('0X')
        ? parseInt(token.value, 16)
        : parseInt(token.value, 10);
      return { kind: 'int', value };
    }
    if (token.type === TokenType.FLOAT) {
      next();
      // As in C, an unsuffixed literal is a double and `f` makes it a float.
      const single = /[fF]$/.test(token.value);
      return { kind: 'float', value: Number.parseFloat(token.value), single };
    }
    // Functional cast: int(x), float(x).
    if (atScalar() && peek(1).type === TokenType.PUNCT && peek(1).value === '(') {
      const to = next().value;
      next();
      const inner = parseExpr();
      expect(')');
      return { kind: 'cast', to, operand: inner };
    }
    if (atKeyword('true')) {
      next();
      return { kind: 'bool', value: true };
    }
    if (atKeyword('false')) {
      next();
      return { kind: 'bool', value: false };
    }
    if (token.type === TokenType.IDENT) {
      next();
      if (BUILTINS.has(token.value)) return { kind: 'builtin', name: token.value };
      if (at('(')) throw new ParseError(`Unknown function '${token.value}'`, token.line);
      return { kind: 'var', name: token.value };
    }
    throw new ParseError(`Unexpected '${token.value}' in expression`, token.line);
  }

  function parsePostfix() {
    let left = parsePrimary();
    for (;;) {
      if (at('[')) {
        next();
        const index = parseExpr();
        expect(']');
        left = { kind: 'index', base: left, index };
        continue;
      }
      if (at('.') && left.kind === 'builtin') {
        next();
        const member = expectIdent();
        const mapped = BUILTIN_MEMBERS[left.name];
        if (!mapped || !(member in mapped)) {
          throw new ParseError(`'${left.name}' has no member '${member}'`, peek().line);
        }
        left = { kind: 'call', callee: mapped[member], args: [] };
        continue;
      }
      if (at('++') || at('--')) {
        const op = next().value;
        left = { kind: 'assign', op: op === '++' ? '+=' : '-=', target: left, value: { kind: 'int', value: 1 } };
        continue;
      }
      break;
    }
    return left;
  }

  function parseUnary() {
    if (at('!') || at('-') || at('~')) {
      const op = next().value;
      return { kind: 'unary', op, operand: parseUnary() };
    }
    // C-style cast: (float)x.
    if (at('(') && peek(1).type === TokenType.KEYWORD && SCALARS.has(peek(1).value) && peek(1).value !== 'void'
        && peek(2).type === TokenType.PUNCT && peek(2).value === ')') {
      next();
      const to = next().value;
      next();
      return { kind: 'cast', to, operand: parseUnary() };
    }
    return parsePostfix();
  }

  function parseBinary(minPrecedence) {
    let left = parseUnary();
    for (;;) {
      const token = peek();
      if (token.type !== TokenType.PUNCT) break;
      const precedence = PRECEDENCE[token.value];
      if (precedence === undefined || precedence < minPrecedence) break;
      next();
      const right = parseBinary(precedence + 1);
      left = { kind: 'binary', op: token.value, left, right };
    }
    return left;
  }

  function parseExpr() {
    const left = parseBinary(1);
    if (ASSIGN_OPS.has(peek().value) && peek().type === TokenType.PUNCT) {
      const op = next().value;
      const value = parseExpr();
      return { kind: 'assign', op, target: left, value };
    }
    return left;
  }

  // ---- statements ----
  function parseBlock() {
    expect('{');
    const body = [];
    while (!at('}')) {
      if (peek().type === TokenType.EOF) throw new ParseError("Unterminated block: expected '}'", peek().line);
      body.push(parseStmt());
    }
    expect('}');
    return body;
  }

  function parseStmt() {
    if (at('{')) return { kind: 'block', body: parseBlock() };

    if (atKeyword('if')) {
      next();
      expect('(');
      const cond = parseExpr();
      expect(')');
      // A braceless body is valid CUDA and is used by the reference kernel, so a single
      // statement is accepted and wrapped rather than rejected.
      const then = at('{') ? parseBlock() : [parseStmt()];
      let otherwise = [];
      if (atKeyword('else')) {
        next();
        otherwise = at('{') ? parseBlock() : [parseStmt()];
      }
      return { kind: 'if', cond, then, otherwise };
    }

    if (atKeyword('while')) {
      next();
      expect('(');
      const cond = parseExpr();
      expect(')');
      const body = at('{') ? parseBlock() : [parseStmt()];
      return { kind: 'while', cond, body };
    }

    if (atKeyword('for')) {
      next();
      expect('(');
      let init = null;
      if (at(';')) next();
      else init = parseSimpleStmt();
      const cond = at(';') ? null : parseExpr();
      expect(';');
      const step = at(')') ? null : parseExpr();
      expect(')');
      const body = at('{') ? parseBlock() : [parseStmt()];
      return { kind: 'for', init, cond, step, body };
    }

    if (atKeyword('break') || atKeyword('continue')) {
      const kind = next().value;
      expect(';');
      return { kind };
    }

    if (atKeyword('return')) {
      next();
      if (at(';')) {
        next();
        return { kind: 'return', value: null };
      }
      const value = parseExpr();
      expect(';');
      return { kind: 'return', value };
    }

    return parseSimpleStmt();
  }

  /** A declaration or expression statement, including its ';'. Also the init clause of `for`. */
  function parseSimpleStmt() {
    // Local scalar declaration: int i = 0; (a functional cast like int(x) is an expression)
    if (atScalar() && !(peek(1).type === TokenType.PUNCT && peek(1).value === '(')) {
      const type = next().value;
      const name = expectIdent();
      let init = null;
      if (at('=')) {
        next();
        init = parseExpr();
      }
      expect(';');
      return { kind: 'decl', name, type, init };
    }

    const expr = parseExpr();
    expect(';');
    // An assignment statement keeps the assign node itself, matching the Stmt kinds in ast.js.
    if (expr.kind === 'assign') return expr;
    return { kind: 'expr', value: expr };
  }

  // ---- top level: a single __global__ kernel ----
  if (!atKeyword('__global__')) {
    throw new ParseError(`A kernel must be declared __global__ (found '${peek().value}')`, peek().line);
  }
  next();
  const returnToken = next();
  if (returnToken.type !== TokenType.KEYWORD || !SCALARS.has(returnToken.value)) {
    throw new ParseError(`'${returnToken.value}' is not a supported return type`, returnToken.line);
  }
  const returnType = returnToken.value;
  const name = expectIdent();
  const params = parseParams();
  const body = parseBlock();

  if (peek().type !== TokenType.EOF) {
    throw new ParseError(`Unexpected trailing input '${peek().value}'`, peek().line);
  }
  return { name, returnType, params, body, isGlobal: true };
}
