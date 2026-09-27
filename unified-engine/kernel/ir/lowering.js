/**
 * AST -> IR lowering.
 *
 * Resolves names against C block scopes, assigns every expression a type using C's
 * usual arithmetic conversions, inserts explicit CONVERTs, and turns && / || into
 * short-circuit control flow. Every failure raises LowerError; the IR it returns is
 * fully typed so a backend never has to infer anything.
 */

import {
  Op, Type, SCALAR_TYPES, Builtin, isFloat,
  makeIdCounter, value, instr, kernel,
} from './ir.js';

export class LowerError extends Error {
  constructor(message) {
    super(message);
    this.name = 'LowerError';
  }
}

const ARITHMETIC = { '+': Op.ADD, '-': Op.SUB, '*': Op.MUL, '/': Op.DIV, '%': Op.REM };
const BITWISE = { '&': Op.BIT_AND, '|': Op.BIT_OR, '^': Op.BIT_XOR };
const SHIFTS = { '<<': Op.SHL, '>>': Op.SHR };
const COMPARISONS = {
  '<': Op.CMP_LT, '>': Op.CMP_GT, '<=': Op.CMP_LE,
  '>=': Op.CMP_GE, '==': Op.CMP_EQ, '!=': Op.CMP_NE,
};

/** Builtins the lowering can read. Only the x dimension is launched today. */
const SUPPORTED_BUILTINS = new Set(Object.values(Builtin));

/** Integer promotion: bool arithmetic happens in int. */
const promote = (type) => (type === Type.BOOL ? Type.I32 : type);

/** C's usual arithmetic conversions, restricted to the supported scalars. */
function commonType(a, b) {
  const pa = promote(a);
  const pb = promote(b);
  for (const type of [Type.F64, Type.F32, Type.U32]) {
    if (pa === type || pb === type) return type;
  }
  return Type.I32;
}

function scalarType(name) {
  const type = SCALAR_TYPES[name];
  if (!type) throw new LowerError(`'${name}' is not a supported value type`);
  return type;
}

export function lower(ast) {
  const ids = makeIdCounter();
  const isVoid = ast.returnType === 'void';
  const returnType = isVoid ? null : scalarType(ast.returnType);

  const params = [];
  const usedNames = new Set();
  const paramScope = new Map();
  for (const p of ast.params) {
    if (usedNames.has(p.name)) throw new LowerError(`Duplicate parameter '${p.name}'`);
    usedNames.add(p.name);
    const param = { name: p.name, kind: p.type.kind, type: scalarType(p.type.scalar), isConst: p.type.isConst };
    params.push(param);
    paramScope.set(p.name, param);
  }

  const locals = [];
  const builtins = new Set();
  const scopes = [paramScope];
  let out = [];
  let loopDepth = 0;

  // ---- helpers ----
  const fresh = (type) => value(ids.next(), type);
  const emit = (ins) => {
    out.push(ins);
    return ins.out;
  };
  /** Runs `fn` with a fresh instruction list and returns what it emitted. */
  const collect = (fn) => {
    const saved = out;
    out = [];
    try {
      fn();
      return out;
    } finally {
      out = saved;
    }
  };
  const scoped = (fn) => {
    scopes.push(new Map());
    try {
      return fn();
    } finally {
      scopes.pop();
    }
  };

  function declareLocal(sourceName, type) {
    let name = sourceName;
    for (let n = 1; usedNames.has(name); n += 1) name = `${sourceName}.${n}`;
    usedNames.add(name);
    const local = { name, kind: 'scalar', type, isConst: false };
    locals.push({ name, type });
    return local;
  }

  function lookup(name) {
    for (let i = scopes.length - 1; i >= 0; i -= 1) {
      const binding = scopes[i].get(name);
      if (binding) return binding;
    }
    throw new LowerError(`Unknown variable '${name}'`);
  }

  const constant = (type, imm) => emit(instr(Op.CONST, { imm }, fresh(type)));

  function convert(val, to) {
    if (val.type === to) return val;
    if (to === Type.BOOL) return toBool(val);
    return emit(instr(Op.CONVERT, { value: val }, fresh(to)));
  }

  function toBool(val) {
    if (val.type === Type.BOOL) return val;
    const zero = constant(val.type, 0);
    return emit(instr(Op.CMP_NE, { left: val, right: zero }, fresh(Type.BOOL)));
  }

  function requireInteger(val, what) {
    if (isFloat(val.type)) throw new LowerError(`${what} requires integer operands`);
  }

  function readBuiltin(name) {
    if (!SUPPORTED_BUILTINS.has(name)) {
      throw new LowerError(`Builtin '${name}' is not supported; only the x dimension is available`);
    }
    if (!isVoid) throw new LowerError(`Builtin '${name}' is only available in void kernels`);
    builtins.add(name);
    return emit(instr(Op.BUILTIN, { name }, fresh(Type.I32)));
  }

  function bufferBinding(expr) {
    if (expr.kind !== 'var') throw new LowerError('Only a buffer parameter can be indexed');
    const binding = lookup(expr.name);
    if (binding.kind !== 'buffer') throw new LowerError(`'${expr.name}' is not a buffer`);
    return binding;
  }

  function lowerIndex(expr) {
    const index = lowerExpr(expr);
    requireInteger(index, 'A buffer index');
    return convert(index, Type.I32);
  }

  /** Applies a binary operator to two already-lowered values. */
  function binaryOp(op, left, right) {
    if (op in ARITHMETIC) {
      const type = commonType(left.type, right.type);
      if (op === '%' && isFloat(type)) throw new LowerError("'%' requires integer operands");
      const l = convert(left, type);
      const r = convert(right, type);
      return emit(instr(ARITHMETIC[op], { left: l, right: r }, fresh(type)));
    }
    if (op in BITWISE) {
      requireInteger(left, `'${op}'`);
      requireInteger(right, `'${op}'`);
      const type = commonType(left.type, right.type);
      return emit(instr(BITWISE[op], { left: convert(left, type), right: convert(right, type) }, fresh(type)));
    }
    if (op in SHIFTS) {
      requireInteger(left, `'${op}'`);
      requireInteger(right, `'${op}'`);
      // The result has the promoted type of the left operand, not the common type.
      const type = promote(left.type);
      return emit(instr(SHIFTS[op], { left: convert(left, type), right: convert(right, type) }, fresh(type)));
    }
    if (op in COMPARISONS) {
      const type = commonType(left.type, right.type);
      const l = convert(left, type);
      const r = convert(right, type);
      return emit(instr(COMPARISONS[op], { left: l, right: r }, fresh(Type.BOOL)));
    }
    throw new LowerError(`Unsupported operator '${op}'`);
  }

  /** a && b / a || b: the right side runs only when it can change the result. */
  function shortCircuit(expr) {
    const result = declareLocal('$sc', Type.BOOL);
    const left = toBool(lowerExpr(expr.left));
    emit(instr(Op.STORE, { name: result.name, value: left }));
    const evalRight = collect(() => {
      const right = toBool(lowerExpr(expr.right));
      emit(instr(Op.STORE, { name: result.name, value: right }));
    });
    const isAnd = expr.op === '&&';
    emit(instr(Op.IF, { cond: left, then: isAnd ? evalRight : [], otherwise: isAnd ? [] : evalRight }));
    return emit(instr(Op.LOAD, { name: result.name }, fresh(Type.BOOL)));
  }

  function lowerAssign(expr) {
    const compound = expr.op === '=' ? null : expr.op.slice(0, -1);

    if (expr.target.kind === 'var') {
      const binding = lookup(expr.target.name);
      if (binding.kind === 'buffer') throw new LowerError(`Cannot assign to buffer '${expr.target.name}'`);
      if (binding.isConst) throw new LowerError(`Cannot assign to const '${expr.target.name}'`);
      let result = lowerExpr(expr.value);
      if (compound) {
        const current = emit(instr(Op.LOAD, { name: binding.name }, fresh(binding.type)));
        result = binaryOp(compound, current, result);
      }
      result = convert(result, binding.type);
      emit(instr(Op.STORE, { name: binding.name, value: result }));
      return result;
    }

    if (expr.target.kind === 'index') {
      const binding = bufferBinding(expr.target.base);
      if (binding.isConst) throw new LowerError(`Cannot write to const buffer '${binding.name}'`);
      const index = lowerIndex(expr.target.index);
      let result = lowerExpr(expr.value);
      if (compound) {
        const current = emit(instr(Op.BUFFER_LOAD, { buffer: binding.name, index }, fresh(binding.type)));
        result = binaryOp(compound, current, result);
      }
      result = convert(result, binding.type);
      emit(instr(Op.BUFFER_STORE, { buffer: binding.name, index, value: result }));
      return result;
    }

    throw new LowerError('The left side of an assignment must be a variable or a buffer element');
  }

  function lowerExpr(expr) {
    switch (expr.kind) {
      case 'int': {
        if (expr.value <= 0x7fffffff) return constant(Type.I32, expr.value);
        if (expr.value <= 0xffffffff) return constant(Type.U32, expr.value);
        throw new LowerError(`Integer literal ${expr.value} does not fit in 32 bits`);
      }
      case 'float':
        return constant(expr.single ? Type.F32 : Type.F64, expr.value);
      case 'bool':
        return constant(Type.BOOL, expr.value ? 1 : 0);
      case 'var': {
        const binding = lookup(expr.name);
        if (binding.kind === 'buffer') throw new LowerError(`Buffer '${expr.name}' must be indexed`);
        return emit(instr(Op.LOAD, { name: binding.name }, fresh(binding.type)));
      }
      case 'builtin':
        return readBuiltin(expr.name);
      case 'call':
        // The parser maps member builtins such as threadIdx.x onto call nodes.
        if (expr.args.length !== 0) throw new LowerError(`Unknown function '${expr.callee}'`);
        return readBuiltin(expr.callee);
      case 'index': {
        const binding = bufferBinding(expr.base);
        const index = lowerIndex(expr.index);
        return emit(instr(Op.BUFFER_LOAD, { buffer: binding.name, index }, fresh(binding.type)));
      }
      case 'unary': {
        const operand = lowerExpr(expr.operand);
        if (expr.op === '!') {
          return emit(instr(Op.NOT, { operand: toBool(operand) }, fresh(Type.BOOL)));
        }
        if (expr.op === '-') {
          const type = promote(operand.type);
          return emit(instr(Op.NEG, { operand: convert(operand, type) }, fresh(type)));
        }
        if (expr.op === '~') {
          requireInteger(operand, "'~'");
          const type = promote(operand.type);
          return emit(instr(Op.BIT_NOT, { operand: convert(operand, type) }, fresh(type)));
        }
        throw new LowerError(`Unsupported unary operator '${expr.op}'`);
      }
      case 'binary':
        if (expr.op === '&&' || expr.op === '||') return shortCircuit(expr);
        return binaryOp(expr.op, lowerExpr(expr.left), lowerExpr(expr.right));
      case 'assign':
        return lowerAssign(expr);
      case 'cast':
        return convert(lowerExpr(expr.operand), scalarType(expr.to));
      default:
        throw new LowerError(`Unsupported expression '${expr.kind}'`);
    }
  }

  function lowerLoopBody(stmts) {
    loopDepth += 1;
    try {
      return collect(() => scoped(() => lowerStmts(stmts)));
    } finally {
      loopDepth -= 1;
    }
  }

  function lowerStmt(stmt) {
    switch (stmt.kind) {
      case 'decl': {
        const type = scalarType(stmt.type);
        if (scopes.at(-1).has(stmt.name)) throw new LowerError(`'${stmt.name}' is already declared in this scope`);
        // Evaluated before the name is in scope, so `int x = x;` reads an outer x.
        // Without an initializer the local is zeroed so loop iterations stay deterministic.
        const init = stmt.init ? convert(lowerExpr(stmt.init), type) : constant(type, 0);
        const local = declareLocal(stmt.name, type);
        scopes.at(-1).set(stmt.name, local);
        emit(instr(Op.STORE, { name: local.name, value: init }));
        return;
      }
      case 'assign':
        lowerAssign(stmt);
        return;
      case 'expr':
        lowerExpr(stmt.value);
        return;
      case 'block':
        scoped(() => lowerStmts(stmt.body));
        return;
      case 'if': {
        const cond = toBool(lowerExpr(stmt.cond));
        const then = collect(() => scoped(() => lowerStmts(stmt.then)));
        const otherwise = collect(() => scoped(() => lowerStmts(stmt.otherwise)));
        emit(instr(Op.IF, { cond, then, otherwise }));
        return;
      }
      case 'while': {
        let cond = null;
        const head = collect(() => { cond = toBool(lowerExpr(stmt.cond)); });
        const body = lowerLoopBody(stmt.body);
        emit(instr(Op.LOOP, { head, cond, body, step: [] }));
        return;
      }
      case 'for':
        scoped(() => {
          if (stmt.init) lowerStmt(stmt.init);
          let cond = null;
          const head = collect(() => { if (stmt.cond) cond = toBool(lowerExpr(stmt.cond)); });
          const body = lowerLoopBody(stmt.body);
          const step = collect(() => { if (stmt.step) lowerExpr(stmt.step); });
          emit(instr(Op.LOOP, { head, cond, body, step }));
        });
        return;
      case 'break':
      case 'continue':
        if (loopDepth === 0) throw new LowerError(`'${stmt.kind}' outside of a loop`);
        emit(instr(stmt.kind === 'break' ? Op.BREAK : Op.CONTINUE));
        return;
      case 'return': {
        if (isVoid) {
          if (stmt.value) throw new LowerError('A void kernel cannot return a value');
          emit(instr(Op.RET, { value: null }));
          return;
        }
        if (!stmt.value) throw new LowerError(`Kernel '${ast.name}' must return a ${ast.returnType}`);
        emit(instr(Op.RET, { value: convert(lowerExpr(stmt.value), returnType) }));
        return;
      }
      default:
        throw new LowerError(`Unsupported statement '${stmt.kind}'`);
    }
  }

  function lowerStmts(stmts) {
    for (const stmt of stmts) lowerStmt(stmt);
  }

  const body = collect(() => scoped(() => lowerStmts(ast.body)));
  return kernel(ast.name, returnType, params, locals, body, [...builtins]);
}
