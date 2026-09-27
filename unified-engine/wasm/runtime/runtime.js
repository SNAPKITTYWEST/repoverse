/**
 * Host runtime for compiled kernels.
 *
 * Owns one WebAssembly.Memory shared by every kernel it loads, hands out typed
 * device buffers from it with a bump allocator, and validates arguments before a
 * launch so a type mismatch fails in JavaScript instead of corrupting memory.
 */

import { compile } from '../../kernel/compiler/compile.js';
import { Type } from '../../kernel/ir/ir.js';
import { MEMORY_IMPORT, ELEMENT, elementTypeOf } from '../abi/abi.js';

const PAGE_BYTES = 65536;
const MAX_I32 = 0x7fffffff;
/** Offset 0 is never handed out, so a zero pointer is always a bug. */
const HEAP_BASE = 16;

export class RuntimeError extends Error {
  constructor(message) {
    super(message);
    this.name = 'RuntimeError';
  }
}

export class DeviceBuffer {
  constructor(runtime, ptr, length, type) {
    this.runtime = runtime;
    this.ptr = ptr;
    this.length = length;
    this.type = type;
    this.generation = runtime.generation;
  }

  get byteLength() {
    return this.length * ELEMENT[this.type].bytes;
  }

  /** A live view over device memory. Invalidated when memory grows; prefer read(). */
  view() {
    this.runtime.checkBuffer(this);
    return new ELEMENT[this.type].array(this.runtime.memory.buffer, this.ptr, this.length);
  }

  read() {
    return this.view().slice();
  }

  write(data) {
    if (data.length !== this.length) {
      throw new RuntimeError(`Expected ${this.length} elements but got ${data.length}`);
    }
    this.view().set(data);
  }
}

export class KernelRuntime {
  constructor({ initialPages = 1, maximumPages = 16384 } = {}) {
    this.memory = new WebAssembly.Memory({ initial: initialPages, maximum: maximumPages });
    this.top = HEAP_BASE;
    this.generation = 0;
  }

  /** Allocates `length` elements of IR type `type` ('i32', 'u32', 'f32', 'f64'), zeroed. */
  alloc(type, length) {
    const element = ELEMENT[type];
    if (!element) throw new RuntimeError(`Unsupported buffer type '${type}'`);
    if (!Number.isInteger(length) || length < 0) throw new RuntimeError(`Invalid buffer length ${length}`);
    const ptr = Math.ceil(this.top / 8) * 8;
    const end = ptr + length * element.bytes;
    if (end > MAX_I32) throw new RuntimeError('Out of device memory');
    const needed = Math.ceil(end / PAGE_BYTES) - this.memory.buffer.byteLength / PAGE_BYTES;
    if (needed > 0) {
      try {
        this.memory.grow(needed);
      } catch {
        throw new RuntimeError('Out of device memory');
      }
    }
    this.top = end;
    const buffer = new DeviceBuffer(this, ptr, length, type);
    // Memory is reused after reset(), so clear it rather than trusting it is zero.
    new Uint8Array(this.memory.buffer, ptr, buffer.byteLength).fill(0);
    return buffer;
  }

  /** Allocates a buffer matching a typed array and copies it in. */
  upload(array) {
    const type = elementTypeOf(array);
    if (!type) throw new RuntimeError(`Unsupported array type ${array?.constructor?.name}`);
    const buffer = this.alloc(type, array.length);
    buffer.write(array);
    return buffer;
  }

  /** Frees every buffer. Existing DeviceBuffers become invalid. */
  reset() {
    this.top = HEAP_BASE;
    this.generation += 1;
  }

  checkBuffer(buffer) {
    if (buffer.runtime !== this) throw new RuntimeError('Buffer belongs to a different runtime');
    if (buffer.generation !== this.generation) throw new RuntimeError('Buffer was freed by reset()');
  }

  /** Compiles kernel source and instantiates it against this runtime's memory. */
  async load(source) {
    const { ir, bytes } = compile(source);
    const imports = { [MEMORY_IMPORT.module]: { [MEMORY_IMPORT.name]: this.memory } };
    const { instance } = await WebAssembly.instantiate(bytes, imports);
    return new Kernel(this, ir, bytes, instance.exports[ir.name]);
  }
}

export class Kernel {
  constructor(runtime, ir, bytes, fn) {
    this.runtime = runtime;
    this.ir = ir;
    this.bytes = bytes;
    this.fn = fn;
  }

  get name() {
    return this.ir.name;
  }

  /** Runs a void kernel on grid × block threads: launch({ grid, block }, ...args). */
  launch({ grid, block }, ...args) {
    if (this.ir.returnType !== null) throw new RuntimeError(`'${this.name}' returns a value; use call()`);
    for (const [label, n] of [['grid', grid], ['block', block]]) {
      if (!Number.isInteger(n) || n < 0 || n > MAX_I32) throw new RuntimeError(`Invalid ${label} size ${n}`);
    }
    if (grid * block > Number.MAX_SAFE_INTEGER) throw new RuntimeError('Launch is too large');
    this.fn(...this.marshal(args), grid, block);
  }

  /** Calls a value-returning kernel once and returns its result. */
  call(...args) {
    if (this.ir.returnType === null) throw new RuntimeError(`'${this.name}' is a void kernel; use launch()`);
    const result = this.fn(...this.marshal(args));
    return this.ir.returnType === Type.U32 ? result >>> 0 : result;
  }

  marshal(args) {
    const { params } = this.ir;
    if (args.length !== params.length) {
      throw new RuntimeError(`'${this.name}' takes ${params.length} arguments but got ${args.length}`);
    }
    return params.map((param, i) => {
      const arg = args[i];
      if (param.kind === 'buffer') {
        if (!(arg instanceof DeviceBuffer)) throw new RuntimeError(`Argument '${param.name}' must be a DeviceBuffer`);
        this.runtime.checkBuffer(arg);
        if (arg.type !== param.type) {
          throw new RuntimeError(`Argument '${param.name}' must be a ${param.type} buffer, not ${arg.type}`);
        }
        return arg.ptr;
      }
      if (typeof arg !== 'number') throw new RuntimeError(`Argument '${param.name}' must be a number`);
      if (param.type === Type.I32 || param.type === Type.U32) {
        const [min, max] = param.type === Type.I32 ? [-0x80000000, MAX_I32] : [0, 0xffffffff];
        if (!Number.isInteger(arg) || arg < min || arg > max) {
          throw new RuntimeError(`Argument '${param.name}' must be a ${param.type} but got ${arg}`);
        }
      }
      return arg;
    });
  }
}
