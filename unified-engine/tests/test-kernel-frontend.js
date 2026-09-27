import { test } from 'node:test';
import assert from 'node:assert/strict';
import { tokenize, TokenType, LexError } from '../kernel/compiler/lexer.js';
import { parse, ParseError } from '../kernel/compiler/parser.js';

const VECTOR_ADD = `
__global__
void vector_add(const float* A, const float* B, float* C, int N)
{
    int i = blockIdx.x * blockDim.x + threadIdx.x;

    if (i < N)
        C[i] = A[i] + B[i];
}
`;

test('lexer: emits keywords, identifiers, numbers and punctuation', () => {
  const tokens = tokenize('int i = 42;');
  assert.equal(tokens[0].type, TokenType.KEYWORD);
  assert.equal(tokens[0].value, 'int');
  assert.equal(tokens[1].type, TokenType.IDENT);
  assert.equal(tokens[3].type, TokenType.INT);
  assert.equal(tokens[3].value, '42');
  assert.equal(tokens.at(-1).type, TokenType.EOF);
});

test('lexer: distinguishes float from int literals', () => {
  assert.equal(tokenize('1.5')[0].type, TokenType.FLOAT);
  assert.equal(tokenize('1')[0].type, TokenType.INT);
  assert.equal(tokenize('2.5f')[0].type, TokenType.FLOAT);
  assert.equal(tokenize('1e3')[0].type, TokenType.FLOAT);
});

test('lexer: parses hex literals', () => {
  const token = tokenize('0x1F')[0];
  assert.equal(token.type, TokenType.INT);
  assert.equal(parseInt(token.value, 16), 31);
});

test('lexer: skips line and block comments', () => {
  const tokens = tokenize('int /* mid */ i; // trailing\n');
  assert.equal(tokens.filter((t) => t.type === TokenType.IDENT).length, 1);
});

test('lexer: tracks line numbers across newlines', () => {
  const tokens = tokenize('int a;\nint b;\nint c;');
  assert.equal(tokens[0].line, 1);
  const cToken = tokens.find((t) => t.value === 'c');
  assert.equal(cToken.line, 3);
});

test('lexer: prefers the longest punctuator', () => {
  const values = tokenize('a += b').map((t) => t.value);
  assert.ok(values.includes('+='));
  assert.ok(!values.includes('+=') === false);
  const shift = tokenize('a << b').map((t) => t.value);
  assert.ok(shift.includes('<<'));
});

test('lexer: rejects an unterminated block comment', () => {
  assert.throws(() => tokenize('int a; /* never closed'), LexError);
});

test('lexer: rejects an unknown character', () => {
  assert.throws(() => tokenize('int a = $;'), LexError);
});

test('parser: produces the expected kernel signature', () => {
  const ast = parse(VECTOR_ADD);
  assert.equal(ast.name, 'vector_add');
  assert.equal(ast.returnType, 'void');
  assert.equal(ast.isGlobal, true);
  assert.equal(ast.params.length, 4);
  assert.deepEqual(ast.params.map((p) => p.name), ['A', 'B', 'C', 'N']);
  assert.deepEqual(ast.params.map((p) => p.type.kind), ['buffer', 'buffer', 'buffer', 'scalar']);
  assert.equal(ast.params[0].type.isConst, true);
  assert.equal(ast.params[2].type.isConst, false);
});

test('parser: lowers thread indexing to global_id_x', () => {
  const ast = parse(VECTOR_ADD);
  const [decl] = ast.body;
  assert.equal(decl.kind, 'decl');
  assert.equal(decl.name, 'i');
  // i = blockIdx.x * blockDim.x + threadIdx.x
  const add = decl.init;
  assert.equal(add.kind, 'binary');
  assert.equal(add.op, '+');
  const mul = add.left;
  assert.equal(mul.kind, 'binary');
  assert.equal(mul.op, '*');
  assert.equal(mul.left.callee, 'block_id_x');
  assert.equal(mul.right.callee, 'block_dim_x');
  assert.equal(add.right.callee, 'local_id_x');
});

test('parser: keeps the guarded assignment as an if statement', () => {
  const ast = parse(VECTOR_ADD);
  const branch = ast.body[1];
  assert.equal(branch.kind, 'if');
  assert.equal(branch.cond.op, '<');
  const assign = branch.then[0];
  assert.equal(assign.kind, 'assign');
  assert.equal(assign.target.kind, 'index');
  assert.equal(assign.value.kind, 'binary');
  assert.equal(assign.value.op, '+');
});

test('parser: accepts explicit casts and unary operators', () => {
  const ast = parse('__global__ int f(int a) { return -int(a) * 2; }');
  // Unary minus binds tighter than '*', as in C: (-int(a)) * 2.
  const ret = ast.body[0].value;
  assert.equal(ret.kind, 'binary');
  assert.equal(ret.op, '*');
  assert.equal(ret.left.kind, 'unary');
  assert.equal(ret.left.operand.kind, 'cast');
  assert.equal(ret.left.operand.to, 'int');
});

test('parser: accepts C-style casts', () => {
  const ast = parse('__global__ float f(int a) { return (float)a / 2.0f; }');
  const ret = ast.body[0].value;
  assert.equal(ret.op, '/');
  assert.equal(ret.left.kind, 'cast');
  assert.equal(ret.left.to, 'float');
  assert.equal(ret.right.single, true);
});

test('parser: follows C precedence for bitwise and shift operators', () => {
  const ast = parse('__global__ int f(int a) { return a & 1 << 2 | 3; }');
  const ret = ast.body[0].value;
  assert.equal(ret.op, '|');
  assert.equal(ret.left.op, '&');
  assert.equal(ret.left.right.op, '<<');
});

test('parser: parses for loops with break and continue', () => {
  const ast = parse('__global__ void f(int n) { for (int i = 0; i < n; i += 1) { if (i == 2) continue; break; } }');
  const loop = ast.body[0];
  assert.equal(loop.kind, 'for');
  assert.equal(loop.init.kind, 'decl');
  assert.equal(loop.cond.op, '<');
  assert.equal(loop.step.kind, 'assign');
  assert.equal(loop.body[0].then[0].kind, 'continue');
  assert.equal(loop.body[1].kind, 'break');
});

test('parser: rejects a kernel without __global__', () => {
  assert.throws(() => parse('void f() { }'), ParseError);
});

test('parser: rejects an unsupported parameter type', () => {
  assert.throws(() => parse('__global__ void f(double2 v) { }'), ParseError);
});

test('parser: rejects trailing input after the kernel', () => {
  assert.throws(() => parse(VECTOR_ADD + '\nint extra;'), ParseError);
});

test('parser: rejects an unterminated body', () => {
  assert.throws(() => parse('__global__ void f() { int i = 0;'), ParseError);
});
