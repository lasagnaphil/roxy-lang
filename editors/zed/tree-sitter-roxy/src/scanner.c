// External scanner for Roxy.
//
// Handles the two things the grammar DSL can't express, both mirroring
// src/roxy/shared/lexer.cpp:
//
//   1. Nested block comments  /* a /* b */ c */
//   2. f-string segmentation. The Roxy lexer emits FStringBegin (f"..{),
//      FStringMid (}..{) and FStringEnd (}.."), and degenerates an
//      interpolation-free f"..." to a plain StringLiteral (lexer.cpp:475).
//      We reproduce that exactly, including the degenerate case, so that
//      `f"plain"` is a string_literal and never opens an fstring node.

#include "tree_sitter/parser.h"

enum TokenType {
  BLOCK_COMMENT,
  STRING_LITERAL,
  FSTRING_BEGIN,
  FSTRING_MID,
  FSTRING_END,
  ERROR_SENTINEL,
};

void *tree_sitter_roxy_external_scanner_create(void) { return NULL; }
void tree_sitter_roxy_external_scanner_destroy(void *payload) { (void)payload; }
unsigned tree_sitter_roxy_external_scanner_serialize(void *payload, char *buffer) {
  (void)payload;
  (void)buffer;
  return 0;
}
void tree_sitter_roxy_external_scanner_deserialize(void *payload, const char *buffer,
                                                   unsigned length) {
  (void)payload;
  (void)buffer;
  (void)length;
}

static void advance(TSLexer *lexer) { lexer->advance(lexer, false); }
static void skip(TSLexer *lexer) { lexer->advance(lexer, true); }

// /* ... */ with nesting.
static bool scan_block_comment(TSLexer *lexer) {
  if (lexer->lookahead != '/') return false;
  advance(lexer);
  if (lexer->lookahead != '*') return false;
  advance(lexer);

  unsigned depth = 1;
  while (depth > 0) {
    if (lexer->eof(lexer)) return false;
    if (lexer->lookahead == '/') {
      advance(lexer);
      if (lexer->lookahead == '*') {
        advance(lexer);
        depth++;
      }
    } else if (lexer->lookahead == '*') {
      advance(lexer);
      if (lexer->lookahead == '/') {
        advance(lexer);
        depth--;
      }
    } else {
      advance(lexer);
    }
  }
  lexer->result_symbol = BLOCK_COMMENT;
  return true;
}

// Scan the text body of an f-string segment, starting just after the opening
// delimiter (f" or }). Returns the token type that terminated it, or -1.
//
//   ... {   -> begin_kind (FSTRING_BEGIN / FSTRING_MID)
//   ... "   -> FSTRING_END, or STRING_LITERAL if this was a `f"..."` with no
//              interpolation at all (only possible when is_begin).
static int scan_fstring_body(TSLexer *lexer, bool is_begin) {
  for (;;) {
    if (lexer->eof(lexer)) return -1;

    if (lexer->lookahead == '\\') {
      advance(lexer);
      if (lexer->eof(lexer)) return -1;
      advance(lexer);  // consume the escaped char, incl. \{ and \}
      continue;
    }
    if (lexer->lookahead == '{') {
      advance(lexer);
      return is_begin ? FSTRING_BEGIN : FSTRING_MID;
    }
    if (lexer->lookahead == '"') {
      advance(lexer);
      return is_begin ? STRING_LITERAL : FSTRING_END;
    }
    advance(lexer);
  }
}

// "..." with escapes.
static bool scan_plain_string(TSLexer *lexer) {
  advance(lexer);  // opening "
  for (;;) {
    if (lexer->eof(lexer)) return false;
    if (lexer->lookahead == '\\') {
      advance(lexer);
      if (lexer->eof(lexer)) return false;
      advance(lexer);
      continue;
    }
    if (lexer->lookahead == '"') {
      advance(lexer);
      lexer->result_symbol = STRING_LITERAL;
      return true;
    }
    advance(lexer);
  }
}

bool tree_sitter_roxy_external_scanner_scan(void *payload, TSLexer *lexer,
                                            const bool *valid_symbols) {
  (void)payload;

  // Bail out inside error recovery: let the parser's own recovery drive.
  if (valid_symbols[ERROR_SENTINEL]) return false;

  // A '}' continuing an interpolation. Only valid when the parser is actually
  // inside an f-string, which keeps struct literals nested in an interpolation
  // (f"{P { x = 1 }}") from being mis-scanned — there the parser expects '}'
  // and neither MID nor END is in valid_symbols.
  if ((valid_symbols[FSTRING_MID] || valid_symbols[FSTRING_END]) && lexer->lookahead == '}') {
    advance(lexer);
    int kind = scan_fstring_body(lexer, false);
    if (kind < 0) return false;
    if (kind == FSTRING_MID && !valid_symbols[FSTRING_MID]) return false;
    if (kind == FSTRING_END && !valid_symbols[FSTRING_END]) return false;
    lexer->result_symbol = kind;
    return true;
  }

  while (lexer->lookahead == ' ' || lexer->lookahead == '\t' || lexer->lookahead == '\r' ||
         lexer->lookahead == '\n') {
    skip(lexer);
  }

  if (valid_symbols[BLOCK_COMMENT] && lexer->lookahead == '/') {
    return scan_block_comment(lexer);
  }

  if (valid_symbols[FSTRING_BEGIN] || valid_symbols[STRING_LITERAL]) {
    if (lexer->lookahead == 'f') {
      advance(lexer);
      if (lexer->lookahead != '"') return false;  // plain identifier starting with f
      advance(lexer);
      int kind = scan_fstring_body(lexer, true);
      if (kind < 0) return false;
      // kind is FSTRING_BEGIN (found '{') or STRING_LITERAL (degenerate f"..").
      if (kind == FSTRING_BEGIN && !valid_symbols[FSTRING_BEGIN]) return false;
      if (kind == STRING_LITERAL && !valid_symbols[STRING_LITERAL]) return false;
      lexer->result_symbol = kind;
      return true;
    }
    if (valid_symbols[STRING_LITERAL] && lexer->lookahead == '"') {
      return scan_plain_string(lexer);
    }
  }

  return false;
}
