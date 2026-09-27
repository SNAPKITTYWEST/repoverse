/**
 * WebAssembly binary encoding primitives: LEB128 integers, IEEE floats, vectors
 * and sections, plus the opcodes the kernel backend emits. Nothing here knows
 * about kernels.
 */

export const ValType = { I32: 0x7f, I64: 0x7e, F32: 0x7d, F64: 0x7c };
export const BLOCK_EMPTY = 0x40;
export const Section = { TYPE: 1, IMPORT: 2, FUNCTION: 3, EXPORT: 7, CODE: 10 };
export const ExternalKind = { FUNC: 0x00, MEMORY: 0x02 };

export const OpCode = {
  unreachable: 0x00, block: 0x02, loop: 0x03, if: 0x04, else: 0x05, end: 0x0b,
  br: 0x0c, br_if: 0x0d, return: 0x0f, call: 0x10,
  local_get: 0x20, local_set: 0x21,
  i32_load: 0x28, f32_load: 0x2a, f64_load: 0x2b,
  i32_store: 0x36, f32_store: 0x38, f64_store: 0x39,
  i32_const: 0x41, f32_const: 0x43, f64_const: 0x44,
  i32_eqz: 0x45, i32_eq: 0x46, i32_ne: 0x47,
  i32_lt_s: 0x48, i32_lt_u: 0x49, i32_gt_s: 0x4a, i32_gt_u: 0x4b,
  i32_le_s: 0x4c, i32_le_u: 0x4d, i32_ge_s: 0x4e, i32_ge_u: 0x4f,
  f32_eq: 0x5b, f32_ne: 0x5c, f32_lt: 0x5d, f32_gt: 0x5e, f32_le: 0x5f, f32_ge: 0x60,
  f64_eq: 0x61, f64_ne: 0x62, f64_lt: 0x63, f64_gt: 0x64, f64_le: 0x65, f64_ge: 0x66,
  i32_add: 0x6a, i32_sub: 0x6b, i32_mul: 0x6c, i32_div_s: 0x6d, i32_div_u: 0x6e,
  i32_rem_s: 0x6f, i32_rem_u: 0x70, i32_and: 0x71, i32_or: 0x72, i32_xor: 0x73,
  i32_shl: 0x74, i32_shr_s: 0x75, i32_shr_u: 0x76,
  f32_neg: 0x8c, f32_add: 0x92, f32_sub: 0x93, f32_mul: 0x94, f32_div: 0x95,
  f64_neg: 0x9a, f64_add: 0xa0, f64_sub: 0xa1, f64_mul: 0xa2, f64_div: 0xa3,
  f32_convert_i32_s: 0xb2, f32_convert_i32_u: 0xb3, f32_demote_f64: 0xb6,
  f64_convert_i32_s: 0xb7, f64_convert_i32_u: 0xb8, f64_promote_f32: 0xbb,
};

/** Saturating float-to-int truncations, 0xFC-prefixed. They never trap on NaN or overflow. */
export const SatOpCode = {
  i32_trunc_sat_f32_s: 0x00, i32_trunc_sat_f32_u: 0x01,
  i32_trunc_sat_f64_s: 0x02, i32_trunc_sat_f64_u: 0x03,
};

export function uleb(n) {
  const bytes = [];
  let v = n >>> 0;
  do {
    let byte = v & 0x7f;
    v >>>= 7;
    if (v !== 0) byte |= 0x80;
    bytes.push(byte);
  } while (v !== 0);
  return bytes;
}

/** Signed LEB128 of a 32-bit integer. */
export function sleb(n) {
  const bytes = [];
  let v = n | 0;
  for (;;) {
    const byte = v & 0x7f;
    v >>= 7;
    const done = (v === 0 && (byte & 0x40) === 0) || (v === -1 && (byte & 0x40) !== 0);
    bytes.push(done ? byte : byte | 0x80);
    if (done) return bytes;
  }
}

export function f32(n) {
  const view = new DataView(new ArrayBuffer(4));
  view.setFloat32(0, n, true);
  return [...new Uint8Array(view.buffer)];
}

export function f64(n) {
  const view = new DataView(new ArrayBuffer(8));
  view.setFloat64(0, n, true);
  return [...new Uint8Array(view.buffer)];
}

export function name(text) {
  const bytes = [...new TextEncoder().encode(text)];
  return [...uleb(bytes.length), ...bytes];
}

/** A length-prefixed vector of already-encoded items. */
export function vec(items) {
  return [...uleb(items.length), ...items.flat()];
}

export function section(id, bytes) {
  return [id, ...uleb(bytes.length), ...bytes];
}

export function funcType(params, results) {
  return [0x60, ...vec(params.map((p) => [p])), ...vec(results.map((r) => [r]))];
}

/** A function body: local declarations run-length encoded by type, then code. */
export function funcBody(localTypes, code) {
  const runs = [];
  for (const type of localTypes) {
    if (runs.length && runs.at(-1).type === type) runs.at(-1).count += 1;
    else runs.push({ type, count: 1 });
  }
  const body = [...vec(runs.map((r) => [...uleb(r.count), r.type])), ...code, OpCode.end];
  return [...uleb(body.length), ...body];
}

export const MAGIC_AND_VERSION = [0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00];
