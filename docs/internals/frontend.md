# Frontend Architecture

The Roxy frontend consists of a compiler and a planned LSP server that share some components.

## Compiler Pipeline

```
Source → Lexer → Parser → AST → Semantic Analysis → IR Builder → SSA IR → Lowering → Bytecode → VM
```

## Shared Components

- **Lexer** — Token definitions, lexing rules, trivia handling
- **Token/Syntax kinds** — Single source of truth for grammar
- **Type system** — Type definitions, compatibility rules
- **Semantic rules** — Shared validation logic

## Separate Components

| Compiler | LSP |
|----------|-----|
| Fail-fast parser | Error-recovering parser |
| AST (lossy) | CST (lossless, preserves trivia) |
| Batch processing | Incremental, lazy analysis |
| Arena allocation | NodeID + hash map |

## LSP Parser (Planned)

The LSP parser will use error recovery to always produce a tree:

```cpp
// Always returns a node, never nullptr
FunctionDeclSyntax* parse_function() {
    Token fun_kw = expect_or_synthetic(TokenKind::KW_fun);
    Token name = expect_or_synthetic(TokenKind::Identifier);
    // ... always produces tree, may contain ErrorNode children
}
```

### LSP Node Storage

```cpp
class SyntaxTree {
    std::unordered_map<NodeId, std::unique_ptr<SyntaxNode>> nodes;
    
    void replace(NodeId id, std::unique_ptr<SyntaxNode> new_node) {
        nodes[id] = std::move(new_node);  // Easy incremental update
    }
};
```

## Key Design Decisions

| Stage | Approach | Rationale |
|-------|----------|-----------|
| Parsing | Separate compiler/LSP parsers, shared lexer | Compiler can fail-fast; LSP needs error recovery |
| IR | SSA with block arguments (not phi nodes) | Cleaner dataflow, easier lowering |
| Bytecode | Register-based, 32-bit fixed-width | Easy C transpilation, natural SSA lowering |
| Memory | Arena allocation (compiler), NodeID maps (LSP) | Fast batch compile; incremental LSP updates |

## Lexer

The lexer tokenizes Roxy source code:
- Decimal, hex (`0xFF`), binary (`0b1010`), octal (`0o77`) number literals
- Integer suffixes (`u`, `l`, `ul`) and float suffix (`f`)
- String literals (raw tokens with quotes)
- All operators including two-character ones (`::`, `&&`, `||`, `+=`, etc.)
- Line comments (`//`) and nested block comments (`/* */`)
- Keyword recognition via trie-style switch
- Accurate line/column tracking

## Parser

The compiler parser is a recursive descent parser with Pratt parsing for expressions:
- Fail-fast design (stops on first error)
- Produces typed AST nodes
- Arena allocation for all nodes
- **String literal processing**: Strips quotes and handles escape sequences (`\n`, `\t`, `\r`, `\\`, `\"`, `\0`)

## AST

Complete AST node definitions:
- 24 expression types (literals, binary ops, unary ops, calls, etc.)
- 9 statement types (if, while, for, return, block, etc.)
- 5 declaration types (function, variable, struct, enum, import)

## Semantic Analysis

Multi-pass semantic analyzer:
1. **Pass 1**: Collect type declarations (structs, enums)
2. **Pass 2**: Resolve type members and global variables
3. **Pass 3**: Analyze function bodies

Features:
- Symbol resolution with scoped symbol tables
- Type inference and type checking
- Function signature validation
- Error reporting with source locations
- NativeRegistry integration for built-in functions
- Struct slot count computation for memory layout
- Out/inout parameter validation

## Files

- `include/roxy/shared/lexer.hpp` - Lexer class
- `src/roxy/shared/lexer.cpp` - Lexer implementation
- `include/roxy/compiler/parser.hpp` - Parser class
- `src/roxy/compiler/parser.cpp` - Parser implementation
- `include/roxy/compiler/ast.hpp` - AST node definitions
- `include/roxy/compiler/semantic.hpp` - Semantic analyzer
- `src/roxy/compiler/semantic.cpp` - Semantic analysis implementation
- `include/roxy/compiler/symbol_table.hpp` - Symbol table
- `src/roxy/compiler/symbol_table.cpp` - Symbol table implementation
