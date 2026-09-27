/**
 * Kernel IR. Backend-neutral intermediate representation.
 *
 * Values are virtual registers: every instruction that produces a result writes a
 * fresh `out` value `{ id, type }` exactly once. Named variables (parameters and
 * locals) are read and written with LOAD / STORE.
 *
 * Control flow is structured rather than a label/jump graph, so a backend such as
 * WebAssembly can emit it directly:
 *
 *   IF    { cond, then: Instr[], otherwise: Instr[] }
 *   LOOP  { head: Instr[], cond: Value | null, body: Instr[], step: Instr[] }
 *         Evaluates `head` then `cond` before each iteration and exits when it is
 *         false. CONTINUE jumps to `step`; BREAK leaves the innermost loop.
 */
export const Op = {
  CONST: 'const', LOAD: 'load', STORE: 'store',
  ADD: 'add', SUB: 'sub', MUL: 'mul', DIV: 'div', REM: 'rem',
  BIT_AND: 'bit_and', BIT_OR: 'bit_or', BIT_XOR: 'bit_xor', SHL: 'shl', SHR: 'shr',
  CMP_LT: 'cmp_lt', CMP_GT: 'cmp_gt', CMP_LE: 'cmp_le',
  CMP_GE: 'cmp_ge', CMP_EQ: 'cmp_eq', CMP_NE: 'cmp_ne',
  NOT: 'not', NEG: 'neg', BIT_NOT: 'bit_not', CONVERT: 'convert',
  BUILTIN: 'builtin',
  BUFFER_LOAD: 'buffer_load', BUFFER_STORE: 'buffer_store',
  IF: 'if', LOOP: 'loop', BREAK: 'break', CONTINUE: 'continue', RET: 'ret',
};

/** `bool` is the 0/1 result of a comparison; backends store it like an i32. */
export const Type = { I32: 'i32', U32: 'u32', F32: 'f32', F64: 'f64', BOOL: 'bool' };

/** Source scalar names to IR types. */
export const SCALAR_TYPES = { int: Type.I32, uint: Type.U32, float: Type.F32, double: Type.F64 };

/** Thread-indexing builtins a kernel may read. */
export const Builtin = {
  GLOBAL_ID_X: 'global_id_x',
  LOCAL_ID_X: 'local_id_x',
  BLOCK_ID_X: 'block_id_x',
  BLOCK_DIM_X: 'block_dim_x',
  GRID_DIM_X: 'grid_dim_x',
};

export const isFloat = (type) => type === Type.F32 || type === Type.F64;

export function makeIdCounter() {
  let next = 0;
  return { next: () => next++ };
}

export function value(id, type) {
  return { id, type };
}

export function instr(op, fields = {}, out = null) {
  return { op, ...fields, out };
}

/**
 * params:  [{ name, kind: 'scalar' | 'buffer', type, isConst }]
 * locals:  [{ name, type }]  (names are unique; shadowed source names are renamed)
 * builtins: Builtin names the body reads
 */
export function kernel(name, returnType, params, locals, body, builtins) {
  return { name, returnType, params, locals, body, builtins };
}

/** Renders IR as indented text for debugging and snapshot-style tests. */
export function printIr(k) {
  const lines = [];
  const params = k.params.map((p) => `${p.kind === 'buffer' ? `${p.type}*` : p.type} ${p.name}`).join(', ');
  lines.push(`kernel ${k.name}(${params}) -> ${k.returnType ?? 'void'}`);
  for (const local of k.locals) lines.push(`  local ${local.type} ${local.name}`);
  const v = (x) => `%${x.id}`;
  const walk = (body, depth) => {
    const pad = '  '.repeat(depth);
    for (const ins of body) {
      const lhs = ins.out ? `${v(ins.out)}:${ins.out.type} = ` : '';
      switch (ins.op) {
        case Op.CONST: lines.push(`${pad}${lhs}const ${ins.imm}`); break;
        case Op.LOAD: lines.push(`${pad}${lhs}load ${ins.name}`); break;
        case Op.STORE: lines.push(`${pad}store ${ins.name}, ${v(ins.value)}`); break;
        case Op.BUILTIN: lines.push(`${pad}${lhs}builtin ${ins.name}`); break;
        case Op.CONVERT: lines.push(`${pad}${lhs}convert ${v(ins.value)}`); break;
        case Op.BUFFER_LOAD: lines.push(`${pad}${lhs}buffer_load ${ins.buffer}[${v(ins.index)}]`); break;
        case Op.BUFFER_STORE: lines.push(`${pad}buffer_store ${ins.buffer}[${v(ins.index)}], ${v(ins.value)}`); break;
        case Op.IF:
          lines.push(`${pad}if ${v(ins.cond)}`);
          walk(ins.then, depth + 1);
          if (ins.otherwise.length) {
            lines.push(`${pad}else`);
            walk(ins.otherwise, depth + 1);
          }
          lines.push(`${pad}end`);
          break;
        case Op.LOOP:
          lines.push(`${pad}loop`);
          walk(ins.head, depth + 1);
          if (ins.cond) lines.push(`${pad}  while ${v(ins.cond)}`);
          walk(ins.body, depth + 1);
          if (ins.step.length) {
            lines.push(`${pad}  step`);
            walk(ins.step, depth + 1);
          }
          lines.push(`${pad}end`);
          break;
        case Op.RET: lines.push(`${pad}ret${ins.value ? ` ${v(ins.value)}` : ''}`); break;
        case Op.BREAK: case Op.CONTINUE: lines.push(`${pad}${ins.op}`); break;
        default: {
          const args = ins.operand ? [ins.operand] : [ins.left, ins.right];
          lines.push(`${pad}${lhs}${ins.op} ${args.map(v).join(', ')}`);
        }
      }
    }
  };
  walk(k.body, 1);
  return lines.join('\n');
}
