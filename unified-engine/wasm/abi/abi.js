/**
 * The contract between compiled kernel modules and the host runtime.
 *
 * Memory: every module imports one linear memory as `env.memory`, owned by the
 * host. A buffer parameter is passed as an i32 byte offset into that memory.
 *
 * Void kernels: the module exports `<name>(...params, grid_dim_x, block_dim_x)`,
 * which runs the body once per thread, block by block, in ascending thread order.
 * The body itself is an internal function taking `...params` followed by
 * THREAD_PARAMS.
 *
 * Non-void kernels: the body is exported directly as `<name>(...params)` and may
 * not read thread builtins.
 */

import { Type } from '../../kernel/ir/ir.js';

export const MEMORY_IMPORT = { module: 'env', name: 'memory' };

/** Appended, in this order, to the kernel parameters of the internal body function. */
export const THREAD_PARAMS = ['block_id_x', 'local_id_x', 'block_dim_x', 'grid_dim_x'];

/** Appended, in this order, to the kernel parameters of an exported void kernel. */
export const LAUNCH_PARAMS = ['grid_dim_x', 'block_dim_x'];

/** Buffer element layout. `log2` is the shift from element index to byte offset. */
export const ELEMENT = {
  [Type.I32]: { bytes: 4, log2: 2, array: Int32Array },
  [Type.U32]: { bytes: 4, log2: 2, array: Uint32Array },
  [Type.F32]: { bytes: 4, log2: 2, array: Float32Array },
  [Type.F64]: { bytes: 8, log2: 3, array: Float64Array },
};

/** The IR element type stored by a typed array, or undefined if it is not supported. */
export function elementTypeOf(array) {
  return Object.keys(ELEMENT).find((type) => array instanceof ELEMENT[type].array);
}
