/**
 * Lexer for the CUDA-like kernel language.
 *
 * Emits a token stream with source offsets so parser and lowering errors can point
 * at a line. Whitespace and comments are skipped; nothing is evaluated here.
 */

export const TokenType = {
  IDENT: 'ident',
  KEYWORD: 'keyword',
  INT: 'int',
  FLOAT: 'float',
  PUNCT: 'punct',
  EOF: 'eof',
};

const KEYWORDS = new Set([
  '__global__',
  '__device__',
  '__shared__',
  'void',
  'int',
  'uint',
  'float',
  'double',
  'const',
  'return',
  'if',
  'else',
  'while',
  'for',
  'break',
  'continue',
  'true',
  'false',
]);

/** Multi-character operators are matched before single-character ones. */
const PUNCTUATORS = [
  '&&', '||', '==', '!=', '<=', '>=', '<<', '>>', '+=', '-=', '*=', '/=', '%=',
  '++', '--', '->',
  '{', '}', '(', ')', '[', ']', ';', ',', '<', '>', '=', '+', '-', '*', '/', '%', '!', '&', '|', '^', '~', '.',
];

export class LexError extends Error {
  constructor(message, line, column) {
    super(`${message} (line ${line}, column ${column})`);
    this.name = 'LexError';
    this.line = line;
    this.column = column;
  }
}

const isDigit = (c) => c >= '0' && c <= '9';
const isIdentStart = (c) => /[A-Za-z_]/.test(c);
const isIdentPart = (c) => /[A-Za-z0-9_]/.test(c);

export function tokenize(source) {
  const tokens = [];
  let i = 0;
  let line = 1;
  let lineStart = 0;

  const column = (offset) => offset - lineStart + 1;

  while (i < source.length) {
    const c = source[i];

    if (c === '\n') {
      i += 1;
      line += 1;
      lineStart = i;
      continue;
    }
    if (c === ' ' || c === '\t' || c === '\r') {
      i += 1;
      continue;
    }
    // Comments run to end of line.
    if (c === '/' && source[i + 1] === '/') {
      while (i < source.length && source[i] !== '\n') i += 1;
      continue;
    }
    if (c === '/' && source[i + 1] === '*') {
      const startLine = line;
      const startColumn = column(i);
      i += 2;
      for (;;) {
        if (i >= source.length) throw new LexError('Unterminated block comment', startLine, startColumn);
        if (source[i] === '\n') {
          line += 1;
          i += 1;
          lineStart = i;
          continue;
        }
        if (source[i] === '*' && source[i + 1] === '/') {
          i += 2;
          break;
        }
        i += 1;
      }
      continue;
    }

    // Identifiers, keywords and __global__-style qualifiers.
    if (isIdentStart(c)) {
      const start = i;
      while (i < source.length && isIdentPart(source[i])) i += 1;
      const value = source.slice(start, i);
      tokens.push({
        type: KEYWORDS.has(value) ? TokenType.KEYWORD : TokenType.IDENT,
        value,
        line,
        column: column(start),
        offset: start,
      });
      continue;
    }

    // Numbers: decimal and 0x hex, with optional fractional part and f suffix.
    if (isDigit(c) || (c === '.' && isDigit(source[i + 1]))) {
      const start = i;
      let isFloat = false;
      if (c === '0' && (source[i + 1] === 'x' || source[i + 1] === 'X')) {
        i += 2;
        while (i < source.length && /[0-9a-fA-F]/.test(source[i])) i += 1;
      } else {
        while (i < source.length && isDigit(source[i])) i += 1;
        if (source[i] === '.') {
          isFloat = true;
          i += 1;
          while (i < source.length && isDigit(source[i])) i += 1;
        }
        if (source[i] === 'e' || source[i] === 'E') {
          isFloat = true;
          i += 1;
          if (source[i] === '+' || source[i] === '-') i += 1;
          while (i < source.length && isDigit(source[i])) i += 1;
        }
      }
      if (source[i] === 'f' || source[i] === 'F') {
        isFloat = true;
        i += 1;
      }
      const text = source.slice(start, i);
      if (i < source.length && isIdentStart(source[i])) {
        throw new LexError(`Malformed numeric literal '${text}${source[i]}'`, line, column(start));
      }
      tokens.push({ type: isFloat ? TokenType.FLOAT : TokenType.INT, value: text, line, column: column(start), offset: start });
      continue;
    }

    // Punctuators, longest match first.
    const punct = PUNCTUATORS.find((p) => source.startsWith(p, i));
    if (punct) {
      const start = i;
      i += punct.length;
      tokens.push({ type: TokenType.PUNCT, value: punct, line, column: column(start), offset: start });
      continue;
    }

    throw new LexError(`Unexpected character '${c}'`, line, column(i));
  }

  tokens.push({ type: TokenType.EOF, value: '<end of input>', line, column: column(i), offset: i });
  return tokens;
}
