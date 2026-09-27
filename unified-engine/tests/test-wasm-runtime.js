import { test } from 'node:test';
import assert from 'node:assert/strict';
import { compile } from '../kernel/compiler/compile.js';
import { KernelRuntime, RuntimeError } from '../wasm/runtime/runtime.js';

const VECTOR_ADD = `
__global__
void vector_add(const float* A, const float* B, float* C, int N)
{
    int i = blockIdx.x * blockDim.x + threadIdx.x;

    if (i < N)
        C[i] = A[i] + B[i];
}
`;

/** Loads a value-returning kernel and returns a plain function. */
async function scalarKernel(source) {
  const kernel = await new KernelRuntime().load(source);
  return (...args) => kernel.call(...args);
}

test('wasm: compiled modules pass WebAssembly validation', () => {
  assert.ok(WebAssembly.validate(compile(VECTOR_ADD).bytes));
  assert.ok(WebAssembly.validate(compile('__global__ double f(double x) { while (x > 1.0) x = x / 2.0; return x; }').bytes));
});

test('wasm: vector_add computes C = A + B and respects the N guard', async () => {
  const runtime = new KernelRuntime();
  const kernel = await runtime.load(VECTOR_ADD);
  const n = 1000;
  const a = Float32Array.from({ length: n }, (_, i) => i * 0.5);
  const b = Float32Array.from({ length: n }, (_, i) => 1000 - i);
  const A = runtime.upload(a);
  const B = runtime.upload(b);
  // One spare element past N proves threads beyond the guard do not write.
  const C = runtime.alloc('f32', n + 1);
  const block = 256;
  kernel.launch({ grid: Math.ceil(n / block), block }, A, B, C, n);
  const c = C.read();
  for (let i = 0; i < n; i += 1) assert.equal(c[i], Math.fround(a[i] + b[i]));
  assert.equal(c[n], 0);
});

test('wasm: global_id_x covers every thread exactly once', async () => {
  const runtime = new KernelRuntime();
  const kernel = await runtime.load('__global__ void count(int* hits) { hits[global_id_x] += 1; }');
  const hits = runtime.alloc('i32', 3 * 7);
  kernel.launch({ grid: 3, block: 7 }, hits);
  assert.deepEqual([...hits.read()], new Array(21).fill(1));
});

test('wasm: integer arithmetic wraps and divides like C', async () => {
  const f = await scalarKernel('__global__ int f(int a, int b) { return a / b * 1000 + a % b; }');
  assert.equal(f(7, 2), 3001);
  assert.equal(f(-7, 2), -3001, 'division truncates toward zero');
  const wrap = await scalarKernel('__global__ int f(int a) { return a + 1; }');
  assert.equal(wrap(0x7fffffff), -0x80000000);
});

test('wasm: unsigned values use unsigned division, comparison and shifts', async () => {
  const f = await scalarKernel('__global__ uint f(uint a) { return a / 2; }');
  assert.equal(f(0xfffffffe), 0x7fffffff);
  const shr = await scalarKernel('__global__ uint f(uint a) { return a >> 28; }');
  assert.equal(shr(0xf0000000), 0xf);
  const lt = await scalarKernel('__global__ int f(uint a, uint b) { return a < b; }');
  assert.equal(lt(1, 0x80000000), 1);
});

test('wasm: signed right shift is arithmetic', async () => {
  const f = await scalarKernel('__global__ int f(int a) { return a >> 1; }');
  assert.equal(f(-8), -4);
});

test('wasm: casts truncate toward zero and saturate instead of trapping', async () => {
  const f = await scalarKernel('__global__ int f(float x) { return (int)x; }');
  assert.equal(f(2.9), 2);
  assert.equal(f(-2.9), -2);
  assert.equal(f(1e20), 0x7fffffff);
  assert.equal(f(Number.NaN), 0);
  const g = await scalarKernel('__global__ float f(int a, int b) { return float(a) / b; }');
  assert.equal(g(1, 4), 0.25);
});

test('wasm: && and || short-circuit so an out-of-range read never happens', async () => {
  const runtime = new KernelRuntime();
  const kernel = await runtime.load(`
    __global__ void f(const int* A, int* out, int n) {
      int i = global_id_x;
      out[i] = (i < n && A[i] > 0) || i == 0;
    }`);
  const A = runtime.upload(Int32Array.from([0, 5, -1, 7]));
  const out = runtime.alloc('i32', 6);
  kernel.launch({ grid: 1, block: 6 }, A, out, 4);
  assert.deepEqual([...out.read()], [1, 1, 0, 1, 0, 0]);
});

test('wasm: for loops honour break, continue and scoped locals', async () => {
  const f = await scalarKernel(`
    __global__ int f(int n) {
      int sum = 0;
      for (int i = 0; i < n; i += 1) {
        if (i == 7) break;
        if (i % 2 == 0) continue;
        int i2 = i * i;
        sum += i2;
      }
      return sum;
    }`);
  assert.equal(f(100), 1 + 9 + 25);
  assert.equal(f(4), 1 + 9);
  assert.equal(f(0), 0);
});

test('wasm: while loops and early returns', async () => {
  const f = await scalarKernel(`
    __global__ int collatz(int n) {
      int steps = 0;
      while (n != 1) {
        if (steps > 1000) return -1;
        if (n % 2 == 0) n = n / 2; else n = 3 * n + 1;
        steps += 1;
      }
      return steps;
    }`);
  assert.equal(f(1), 0);
  assert.equal(f(27), 111);
});

test('wasm: doubles keep full precision', async () => {
  const f = await scalarKernel('__global__ double f(double a, double b) { return a + b; }');
  assert.equal(f(0.1, 0.2), 0.1 + 0.2);
});

test('wasm: a game particle step integrates and bounces off the floor', async () => {
  const runtime = new KernelRuntime();
  const step = await runtime.load(`
    __global__ void step(float* posY, float* velY, int count, float dt, float gravity) {
      int i = global_id_x;
      if (i >= count) return;
      float v = velY[i] - gravity * dt;
      float y = posY[i] + v * dt;
      if (y < 0.0f) {
        y = -y;
        v = -v * 0.5f;
      }
      posY[i] = y;
      velY[i] = v;
    }`);
  const count = 5;
  const posY = runtime.upload(Float32Array.from([10, 5, 0.01, 2, 0]));
  const velY = runtime.upload(Float32Array.from([0, 1, -2, 0, 0]));
  const dt = 0.016;
  const gravity = 9.8;

  // Reference implementation in JS with the same f32 rounding.
  const y = Float32Array.from(posY.read());
  const v = Float32Array.from(velY.read());
  const f = Math.fround;
  for (let frame = 0; frame < 120; frame += 1) {
    step.launch({ grid: 1, block: 8 }, posY, velY, count, dt, gravity);
    for (let i = 0; i < count; i += 1) {
      let nv = f(v[i] - f(f(gravity) * f(dt)));
      let ny = f(y[i] + f(nv * f(dt)));
      if (ny < 0) {
        ny = -ny;
        nv = f(-nv * 0.5);
      }
      y[i] = ny;
      v[i] = nv;
    }
  }
  assert.deepEqual([...posY.read()], [...y]);
  assert.deepEqual([...velY.read()], [...v]);
  assert.ok([...posY.read()].every((p) => p >= 0), 'nothing falls through the floor');
});

test('runtime: kernels loaded into one runtime share device buffers', async () => {
  const runtime = new KernelRuntime();
  const fill = await runtime.load('__global__ void fill(int* A, int v) { A[global_id_x] = v; }');
  const double = await runtime.load('__global__ void twice(int* A) { A[global_id_x] *= 2; }');
  const A = runtime.alloc('i32', 4);
  fill.launch({ grid: 1, block: 4 }, A, 21);
  double.launch({ grid: 1, block: 4 }, A);
  assert.deepEqual([...A.read()], [42, 42, 42, 42]);
});

test('runtime: grows memory for large buffers', async () => {
  const runtime = new KernelRuntime({ initialPages: 1 });
  const kernel = await runtime.load('__global__ void iota(uint* A) { A[global_id_x] = global_id_x; }');
  const n = 100000;
  const A = runtime.alloc('u32', n);
  kernel.launch({ grid: n / 1000, block: 1000 }, A);
  const out = A.read();
  assert.equal(out[0], 0);
  assert.equal(out[n - 1], n - 1);
});

test('runtime: rejects mismatched launch arguments', async () => {
  const runtime = new KernelRuntime();
  const kernel = await runtime.load(VECTOR_ADD);
  const f = runtime.alloc('f32', 4);
  const i = runtime.alloc('i32', 4);
  const grid = { grid: 1, block: 4 };
  assert.throws(() => kernel.launch(grid, f, f, f), RuntimeError);
  assert.throws(() => kernel.launch(grid, f, f, i, 4), /must be a f32 buffer/);
  assert.throws(() => kernel.launch(grid, f, f, f, 4.5), /must be a i32/);
  assert.throws(() => kernel.launch({ grid: -1, block: 4 }, f, f, f, 4), /Invalid grid/);
  assert.throws(() => kernel.call(f, f, f, 4), /use launch/);
  const other = new KernelRuntime().alloc('f32', 4);
  assert.throws(() => kernel.launch(grid, f, f, other, 4), /different runtime/);
  runtime.reset();
  assert.throws(() => kernel.launch(grid, f, f, f, 4), /freed by reset/);
});

test('runtime: load() rejects a kernel that does not compile', async () => {
  await assert.rejects(new KernelRuntime().load('__global__ void f() { x = 1; }'), /Unknown variable/);
});
