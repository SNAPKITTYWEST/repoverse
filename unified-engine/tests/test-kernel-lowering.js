import { test } from 'node:test';
import assert from 'node:assert/strict';
import { parse } from '../kernel/compiler/parser.js';
import { lower, LowerError } from '../kernel/ir/lowering.js';
import { Op, Type, printIr } from '../kernel/ir/ir.js';

const lowerSource = (source) => lower(parse(source));

/** Every instruction in a body, including nested control flow. */
function flatten(body) {
  return body.flatMap((ins) => [
    ins,
    ...flatten(ins.then ?? []), ...flatten(ins.otherwise ?? []),
    ...flatten(ins.head ?? []), ...flatten(ins.body ?? []), ...flatten(ins.step ?? []),
  ]);
}

const VECTOR_ADD = `
__global__ void vector_add(const float* A, const float* B, float* C, int N) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < N)
        C[i] = A[i] + B[i];
}`;

test('lowering: vector_add has typed params, a guarded store and the builtins it reads', () => {
  const ir = lowerSource(VECTOR_ADD);
  assert.equal(ir.returnType, null);
  assert.deepEqual(ir.params.map((p) => [p.name, p.kind, p.type]), [
    ['A', 'buffer', Type.F32], ['B', 'buffer', Type.F32], ['C', 'buffer', Type.F32], ['N', 'scalar', Type.I32],
  ]);
  assert.deepEqual(ir.builtins.sort(), ['block_dim_x', 'block_id_x', 'local_id_x']);
  const branch = ir.body.find((ins) => ins.op === Op.IF);
  assert.ok(branch);
  const store = branch.then.find((ins) => ins.op === Op.BUFFER_STORE);
  assert.equal(store.buffer, 'C');
  assert.equal(store.value.type, Type.F32);
});

test('lowering: every value id is defined exactly once', () => {
  const ir = lowerSource(VECTOR_ADD);
  const ids = flatten(ir.body).filter((ins) => ins.out).map((ins) => ins.out.id);
  assert.equal(new Set(ids).size, ids.length);
});

test('lowering: applies the usual arithmetic conversions', () => {
  const ir = lowerSource('__global__ float f(int a, float b) { return a * b; }');
  const mul = flatten(ir.body).find((ins) => ins.op === Op.MUL);
  assert.equal(mul.out.type, Type.F32);
  assert.equal(mul.left.type, Type.F32, 'the int operand is converted to float');
});

test('lowering: an unsuffixed float literal is a double', () => {
  const ir = lowerSource('__global__ float f(float x) { return x * 0.5; }');
  const mul = flatten(ir.body).find((ins) => ins.op === Op.MUL);
  assert.equal(mul.out.type, Type.F64);
});

test('lowering: a shift keeps the type of its left operand', () => {
  const ir = lowerSource('__global__ uint f(uint a, int b) { return a >> b; }');
  const shr = flatten(ir.body).find((ins) => ins.op === Op.SHR);
  assert.equal(shr.out.type, Type.U32);
});

test('lowering: && short-circuits through a conditional', () => {
  const ir = lowerSource('__global__ int f(const int* A, int i, int n) { return i < n && A[i] > 0; }');
  const branch = ir.body.find((ins) => ins.op === Op.IF);
  assert.ok(branch.then.some((ins) => ins.op === Op.BUFFER_LOAD), 'A[i] is only read when i < n');
  assert.ok(!ir.body.some((ins) => ins.op === Op.BUFFER_LOAD));
});

test('lowering: renames shadowed locals', () => {
  const ir = lowerSource('__global__ int f() { int x = 1; { int x = 2; } return x; }');
  assert.deepEqual(ir.locals.map((l) => l.name), ['x', 'x.1']);
  const ret = ir.body.at(-1);
  const load = flatten(ir.body).find((ins) => ins.out === ret.value);
  assert.equal(load.name, 'x');
});

test('lowering: printIr renders the kernel', () => {
  const text = printIr(lowerSource(VECTOR_ADD));
  assert.match(text, /^kernel vector_add\(f32\* A, f32\* B, f32\* C, i32 N\) -> void/);
  assert.match(text, /buffer_store C\[%\d+\], %\d+/);
});

for (const [label, source, pattern] of [
  ['an unknown variable', '__global__ void f() { x = 1; }', /Unknown variable 'x'/],
  ['a write to a const buffer', '__global__ void f(const int* A) { A[0] = 1; }', /const buffer 'A'/],
  ['assigning a buffer', '__global__ void f(int* A, int* B) { A = B; }', /assign to buffer/],
  ['a buffer used as a value', '__global__ int f(int* A) { return A; }', /must be indexed/],
  ['a float index', '__global__ void f(int* A) { A[1.0f] = 1; }', /index requires integer/],
  ['float %', '__global__ float f(float a) { return a % 2.0f; }', /'%' requires integer/],
  ['break outside a loop', '__global__ void f() { break; }', /outside of a loop/],
  ['a redeclaration', '__global__ void f() { int a = 1; int a = 2; }', /already declared/],
  ['a value from a void kernel', '__global__ void f() { return 1; }', /cannot return a value/],
  ['a missing return value', '__global__ int f() { return; }', /must return/],
  ['builtins in a value kernel', '__global__ int f() { return threadIdx.x; }', /only available in void kernels/],
  ['the y dimension', '__global__ void f(int* A) { A[0] = threadIdx.y; }', /only the x dimension/],
]) {
  test(`lowering: rejects ${label}`, () => {
    assert.throws(() => lowerSource(source), (error) => error instanceof LowerError && pattern.test(error.message));
  });
}
