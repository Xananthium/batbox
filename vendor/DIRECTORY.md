# vendor

Vendored third-party source code (git submodules). Contains tree-sitter core and language grammar parsers for syntax highlighting.

All source in this directory is upstream/vendored code. Function-level documentation is not provided; refer to the upstream projects for API documentation.

## Files

### CMakeLists.txt
Builds the batbox_treesitter static library from vendored tree-sitter sources. Active only when BATBOX_SYNTAX=ON. Aborts with FATAL_ERROR if submodules have not been checked out.

### Vendored subdirectories (git submodules)
- `tree-sitter/` — tree-sitter runtime core (libtree-sitter): the parsing engine
- `tree-sitter-cpp/` — C++ grammar for tree-sitter
- `tree-sitter-python/` — Python grammar for tree-sitter
- `tree-sitter-javascript/` — JavaScript grammar for tree-sitter
- `tree-sitter-go/` — Go grammar for tree-sitter
- `tree-sitter-rust/` — Rust grammar for tree-sitter
