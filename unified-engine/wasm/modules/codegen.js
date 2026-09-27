/**
 * Kernel IR -> WebAssembly binary.
 *
 * Every IR virtual register becomes a typed wasm local, which keeps the stack
 * discipline trivial: each instruction pushes its operands with local.get and
 * stores its result with local.set. Structured IR control flow maps directly onto
 * wasm block / loop / if. The module layout follows ../abi/abi.js.
 */

import { Op, Type, Builtin, isFloat } from '../../kernel/ir/ir.js';
import { MEMORY_IMPORT, THREAD_PARAMS, LAUNCH_PARAMS, ELEMENT } from '../abi/abi.js';
import {
  ValType, BLOCK_EMPTY, Section, ExternalKind, OpCode, SatOpCode, MAGIC_AND_VERSION,
  uleb, sleb, f32, f64, name, vec, section, funcType, funcBody,
} from './encoder.js';

export class CodegenError extends Error {
  constructor(message) {
    super(message);
    this.name = 'CodegenError';
  }
}

const VAL_TYPE = {
  [Type.I32]: ValType.I32, [Type.U32]: ValType.I32, [Type.BOOL]: ValType.I32,
  [Type.F32]: ValType.F32, [Type.F64]: ValType.F64,
};

/** The wasm instruction family an IR type computes in: i32 (signed), u32, f32 or f64. */
const family = (type) => (type === Type.BOOL ? Type.I32 : type);

const ARITH = {
  [Op.ADD]: { i32: 'i32_add', u32: 'i32_add', f32: 'f32_add', f64: 'f64_add' },
  [Op.SUB]: { i32: 'i32_sub', u32: 'i32_sub', f32: 'f32_sub', f64: 'f64_sub' },
  [Op.MUL]: { i32: 'i32_mul', u32: 'i32_mul', f32: 'f32_mul', f64: 'f64_mul' },
  [Op.DIV]: { i32: 'i32_div_s', u32: 'i32_div_u', f32: 'f32_div', f64: 'f64_div' },
  [Op.REM]: { i32: 'i32_rem_s', u32: 'i32_rem_u' },
  [Op.BIT_AND]: { i32: 'i32_and', u32: 'i32_and' },
  [Op.BIT_OR]: { i32: 'i32_or', u32: 'i32_or' },
  [Op.BIT_XOR]: { i32: 'i32_xor', u32: 'i32_xor' },
  [Op.SHL]: { i32: 'i32_shl', u32: 'i32_shl' },
  [Op.SHR]: { i32: 'i32_shr_s', u32: 'i32_shr_u' },
  [Op.CMP_LT]: { i32: 'i32_lt_s', u32: 'i32_lt_u', f32: 'f32_lt', f64: 'f64_lt' },
  [Op.CMP_GT]: { i32: 'i32_gt_s', u32: 'i32_gt_u', f32: 'f32_gt', f64: 'f64_gt' },
  [Op.CMP_LE]: { i32: 'i32_le_s', u32: 'i32_le_u', f32: 'f32_le', f64: 'f64_le' },
  [Op.CMP_GE]: { i32: 'i32_ge_s', u32: 'i32_ge_u', f32: 'f32_ge', f64: 'f64_ge' },
  [Op.CMP_EQ]: { i32: 'i32_eq', u32: 'i32_eq', f32: 'f32_eq', f64: 'f64_eq' },
  [Op.CMP_NE]: { i32: 'i32_ne', u32: 'i32_ne', f32: 'f32_ne', f64: 'f64_ne' },
};

/** CONVERT opcodes keyed by `${from}>${to}`; an empty list is a reinterpretation. */
const CONVERSIONS = {
  'i32>u32': [], 'u32>i32': [], 'bool>i32': [], 'bool>u32': [],
  'i32>f32': ['f32_convert_i32_s'], 'u32>f32': ['f32_convert_i32_u'], 'bool>f32': ['f32_convert_i32_s'],
  'i32>f64': ['f64_convert_i32_s'], 'u32>f64': ['f64_convert_i32_u'], 'bool>f64': ['f64_convert_i32_s'],
  'f32>f64': ['f64_promote_f32'], 'f64>f32': ['f32_demote_f64'],
  'f32>i32': [{ sat: 'i32_trunc_sat_f32_s' }], 'f32>u32': [{ sat: 'i32_trunc_sat_f32_u' }],
  'f64>i32': [{ sat: 'i32_trunc_sat_f64_s' }], 'f64>u32': [{ sat: 'i32_trunc_sat_f64_u' }],
};

const LOAD = { i32: 'i32_load', u32: 'i32_load', f32: 'f32_load', f64: 'f64_load' };
const STORE = { i32: 'i32_store', u32: 'i32_store', f32: 'f32_store', f64: 'f64_store' };

/** Emits the body function of a kernel. Returns { params, results, locals, code }. */
function emitBody(ir) {
  const isVoid = ir.returnType === null;
  const code = [];
  const localIndex = new Map();
  const params = [];
  const locals = [];

  for (const p of ir.params) {
    localIndex.set(p.name, params.length);
    params.push(p.kind === 'buffer' ? ValType.I32 : VAL_TYPE[p.type]);
  }
  if (isVoid) {
    for (const t of THREAD_PARAMS) {
      localIndex.set(`@${t}`, params.length);
      params.push(ValType.I32);
    }
  }
  const addLocal = (key, valType) => {
    localIndex.set(key, params.length + locals.length);
    locals.push(valType);
  };
  for (const local of ir.locals) addLocal(local.name, VAL_TYPE[local.type]);

  const vreg = (val) => {
    const key = `%${val.id}`;
    if (!localIndex.has(key)) addLocal(key, VAL_TYPE[val.type]);
    return localIndex.get(key);
  };
  const buffers = new Map(ir.params.filter((p) => p.kind === 'buffer').map((p) => [p.name, p]));

  const op = (mnemonic) => code.push(OpCode[mnemonic]);
  const get = (val) => code.push(OpCode.local_get, ...uleb(vreg(val)));
  const set = (val) => code.push(OpCode.local_set, ...uleb(vreg(val)));
  const getNamed = (key) => code.push(OpCode.local_get, ...uleb(localIndex.get(key)));
  const setNamed = (key) => code.push(OpCode.local_set, ...uleb(localIndex.get(key)));

  function pushConst(type, imm) {
    if (type === Type.F32) code.push(OpCode.f32_const, ...f32(imm));
    else if (type === Type.F64) code.push(OpCode.f64_const, ...f64(imm));
    else code.push(OpCode.i32_const, ...sleb(imm));
  }

  /** Pushes the byte address of buffer[index]. */
  function address(bufferName, index) {
    const buffer = buffers.get(bufferName);
    getNamed(buffer.name);
    get(index);
    code.push(OpCode.i32_const, ...sleb(ELEMENT[buffer.type].log2));
    op('i32_shl');
    op('i32_add');
    return buffer;
  }

  /** Memory immediate: alignment hint (log2 bytes) and a zero offset. */
  const memarg = (type) => code.push(...uleb(ELEMENT[type].log2), ...uleb(0));

  // Control stack of labels so BREAK / CONTINUE can compute branch depths.
  const labels = [];
  const depthOf = (kind) => {
    for (let i = labels.length - 1; i >= 0; i -= 1) {
      if (labels[i] === kind) return labels.length - 1 - i;
    }
    throw new CodegenError(`'${kind}' has no enclosing loop`);
  };
  const open = (opcode, label) => {
    code.push(opcode, BLOCK_EMPTY);
    labels.push(label);
  };
  const close = () => {
    op('end');
    labels.pop();
  };

  function emitInstr(ins) {
    switch (ins.op) {
      case Op.CONST:
        pushConst(ins.out.type, ins.imm);
        set(ins.out);
        return;
      case Op.LOAD:
        getNamed(ins.name);
        set(ins.out);
        return;
      case Op.STORE:
        get(ins.value);
        setNamed(ins.name);
        return;
      case Op.BUILTIN:
        if (ins.name === Builtin.GLOBAL_ID_X) {
          getNamed('@block_id_x');
          getNamed('@block_dim_x');
          op('i32_mul');
          getNamed('@local_id_x');
          op('i32_add');
        } else {
          getNamed(`@${ins.name}`);
        }
        set(ins.out);
        return;
      case Op.NOT:
        get(ins.operand);
        op('i32_eqz');
        set(ins.out);
        return;
      case Op.NEG:
        if (isFloat(ins.out.type)) {
          get(ins.operand);
          op(ins.out.type === Type.F32 ? 'f32_neg' : 'f64_neg');
        } else {
          code.push(OpCode.i32_const, 0);
          get(ins.operand);
          op('i32_sub');
        }
        set(ins.out);
        return;
      case Op.BIT_NOT:
        get(ins.operand);
        code.push(OpCode.i32_const, ...sleb(-1));
        op('i32_xor');
        set(ins.out);
        return;
      case Op.CONVERT: {
        const steps = CONVERSIONS[`${ins.value.type}>${ins.out.type}`];
        if (!steps) throw new CodegenError(`No conversion from ${ins.value.type} to ${ins.out.type}`);
        get(ins.value);
        for (const step of steps) {
          if (typeof step === 'string') op(step);
          else code.push(0xfc, ...uleb(SatOpCode[step.sat]));
        }
        set(ins.out);
        return;
      }
      case Op.BUFFER_LOAD: {
        const buffer = address(ins.buffer, ins.index);
        op(LOAD[buffer.type]);
        memarg(buffer.type);
        set(ins.out);
        return;
      }
      case Op.BUFFER_STORE: {
        const buffer = address(ins.buffer, ins.index);
        get(ins.value);
        op(STORE[buffer.type]);
        memarg(buffer.type);
        return;
      }
      case Op.IF:
        get(ins.cond);
        open(OpCode.if, 'if');
        emitList(ins.then);
        if (ins.otherwise.length) {
          op('else');
          emitList(ins.otherwise);
        }
        close();
        return;
      case Op.LOOP:
        // block $break { loop $top { head; br_if $break (!cond); block $continue { body } step; br $top } }
        open(OpCode.block, 'break');
        open(OpCode.loop, 'top');
        emitList(ins.head);
        if (ins.cond) {
          get(ins.cond);
          op('i32_eqz');
          code.push(OpCode.br_if, ...uleb(depthOf('break')));
        }
        open(OpCode.block, 'continue');
        emitList(ins.body);
        close();
        emitList(ins.step);
        code.push(OpCode.br, ...uleb(depthOf('top')));
        close();
        close();
        return;
      case Op.BREAK:
        code.push(OpCode.br, ...uleb(depthOf('break')));
        return;
      case Op.CONTINUE:
        code.push(OpCode.br, ...uleb(depthOf('continue')));
        return;
      case Op.RET:
        if (ins.value) get(ins.value);
        op('return');
        return;
      default: {
        const table = ARITH[ins.op];
        if (!table) throw new CodegenError(`Unsupported IR op '${ins.op}'`);
        const mnemonic = table[family(ins.left.type)];
        if (!mnemonic) throw new CodegenError(`'${ins.op}' is not defined for ${ins.left.type}`);
        get(ins.left);
        get(ins.right);
        op(mnemonic);
        set(ins.out);
      }
    }
  }

  function emitList(list) {
    for (const ins of list) emitInstr(ins);
  }

  emitList(ir.body);
  // A value-returning body must not fall off the end; wasm validation needs the stack typed.
  if (!isVoid) op('unreachable');

  return { params, results: isVoid ? [] : [VAL_TYPE[ir.returnType]], locals, code };
}

/**
 * The exported launcher for a void kernel: loops over blocks then threads and calls
 * the body (function `bodyIndex`) once per thread.
 */
function emitLauncher(ir, bodyIndex) {
  const n = ir.params.length;
  const params = [
    ...ir.params.map((p) => (p.kind === 'buffer' ? ValType.I32 : VAL_TYPE[p.type])),
    ...LAUNCH_PARAMS.map(() => ValType.I32),
  ];
  const grid = n;
  const blockDim = n + 1;
  const block = n + 2;
  const thread = n + 3;
  const get = (i) => [OpCode.local_get, ...uleb(i)];
  const set = (i) => [OpCode.local_set, ...uleb(i)];
  const increment = (i) => [...get(i), OpCode.i32_const, 1, OpCode.i32_add, ...set(i)];

  const call = [];
  for (let i = 0; i < n; i += 1) call.push(...get(i));
  // THREAD_PARAMS order: block_id_x, local_id_x, block_dim_x, grid_dim_x
  call.push(...get(block), ...get(thread), ...get(blockDim), ...get(grid), OpCode.call, ...uleb(bodyIndex));

  const code = [
    OpCode.block, BLOCK_EMPTY, OpCode.loop, BLOCK_EMPTY,
    ...get(block), ...get(grid), OpCode.i32_ge_u, OpCode.br_if, 1,
    OpCode.i32_const, 0, ...set(thread),
    OpCode.block, BLOCK_EMPTY, OpCode.loop, BLOCK_EMPTY,
    ...get(thread), ...get(blockDim), OpCode.i32_ge_u, OpCode.br_if, 1,
    ...call,
    ...increment(thread),
    OpCode.br, 0,
    OpCode.end, OpCode.end,
    ...increment(block),
    OpCode.br, 0,
    OpCode.end, OpCode.end,
  ];
  return { params, results: [], locals: [ValType.I32, ValType.I32], code };
}

/** Compiles one kernel's IR to a complete wasm module. */
export function emitModule(ir) {
  const body = emitBody(ir);
  const functions = [body];
  if (ir.returnType === null) functions.push(emitLauncher(ir, 0));
  const exported = functions.length - 1;

  const bytes = [
    ...MAGIC_AND_VERSION,
    ...section(Section.TYPE, vec(functions.map((f) => funcType(f.params, f.results)))),
    ...section(Section.IMPORT, vec([[
      ...name(MEMORY_IMPORT.module), ...name(MEMORY_IMPORT.name), ExternalKind.MEMORY, 0x00, ...uleb(0),
    ]])),
    ...section(Section.FUNCTION, vec(functions.map((_, i) => uleb(i)))),
    ...section(Section.EXPORT, vec([[...name(ir.name), ExternalKind.FUNC, ...uleb(exported)]])),
    ...section(Section.CODE, vec(functions.map((f) => funcBody(f.locals, f.code)))),
  ];
  return new Uint8Array(bytes);
}
