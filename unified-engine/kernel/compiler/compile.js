/**
 * The full kernel pipeline: source -> AST -> IR -> wasm bytes.
 */

import { parse } from './parser.js';
import { lower } from '../ir/lowering.js';
import { emitModule } from '../../wasm/modules/codegen.js';

export function compile(source) {
  const ast = parse(source);
  const ir = lower(ast);
  const bytes = emitModule(ir);
  return { ast, ir, bytes };
}
