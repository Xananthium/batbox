# vendor/ — Vendored tree-sitter grammars

This directory contains the tree-sitter core library and the 10 language
grammars used by batbox's `BATBOX_SYNTAX` syntax-highlighting path.  Sources
are tracked as **git submodules** — they are not fetched at CMake configure
time and are not managed by vcpkg.

---

## Submodule setup

After cloning the repository, initialize all submodules once:

```sh
git submodule update --init --recursive
```

Submodules are then checked out at the pinned commits recorded in
`.gitmodules` (and in `.git/modules/`).  CMake will emit a `FATAL_ERROR`
with the command above if any submodule is absent when `BATBOX_SYNTAX=ON`.

---

## Vendored components

| Submodule directory              | Language   | Upstream URL                                              |
|----------------------------------|------------|-----------------------------------------------------------|
| `vendor/tree-sitter`             | core lib   | https://github.com/tree-sitter/tree-sitter                |
| `vendor/tree-sitter-c`           | C          | https://github.com/tree-sitter/tree-sitter-c              |
| `vendor/tree-sitter-cpp`         | C++        | https://github.com/tree-sitter/tree-sitter-cpp            |
| `vendor/tree-sitter-python`      | Python     | https://github.com/tree-sitter/tree-sitter-python         |
| `vendor/tree-sitter-javascript`  | JavaScript | https://github.com/tree-sitter/tree-sitter-javascript     |
| `vendor/tree-sitter-typescript`  | TypeScript | https://github.com/tree-sitter/tree-sitter-typescript     |
| `vendor/tree-sitter-rust`        | Rust       | https://github.com/tree-sitter-grammars/tree-sitter-rust  |
| `vendor/tree-sitter-go`          | Go         | https://github.com/tree-sitter-grammars/tree-sitter-go    |
| `vendor/tree-sitter-bash`        | Bash       | https://github.com/tree-sitter-grammars/tree-sitter-bash  |
| `vendor/tree-sitter-json`        | JSON       | https://github.com/tree-sitter-grammars/tree-sitter-json  |
| `vendor/tree-sitter-markdown`    | Markdown   | https://github.com/tree-sitter-grammars/tree-sitter-markdown |

---

## Why we vendor (not vcpkg, not FetchContent, not runtime fetch)

**Non-goal from pmdraft.md**: "No runtime dependency fetching — batbox must
build from a fully checked-out source tree without internet access."

Three specific reasons this drives git-submodule vendoring:

1. **Local-sandbox principle**: batbox is designed for air-gapped and
   offline-capable environments.  A `FetchContent_Declare(... GIT_REPOSITORY
   ...)` or vcpkg online install would silently break those builds.

2. **Version stability**: the vcpkg tree-sitter-core port may lag behind
   upstream releases or pin grammar ABIs that differ from our chosen grammar
   commits.  Submodules let us pin core + each grammar independently and
   advance them deliberately.

3. **Deterministic CI**: every CI run sees the exact same grammar sources
   because the commit SHA is recorded in `.gitmodules` and locked by
   `git submodule update`.  No CDN availability, no version-range drift.

---

## Updating a single grammar

To advance one grammar to its latest `master`:

```sh
git submodule update --remote vendor/tree-sitter-python
git add vendor/tree-sitter-python
git commit -m "chore(vendor): bump tree-sitter-python to latest master"
```

To pin to a specific commit:

```sh
cd vendor/tree-sitter-python
git fetch origin
git checkout <TARGET_COMMIT_SHA>
cd ../..
git add vendor/tree-sitter-python
git commit -m "chore(vendor): pin tree-sitter-python to <TARGET_COMMIT_SHA>"
```

To update all submodules at once:

```sh
git submodule update --remote
git add vendor/
git commit -m "chore(vendor): bump all tree-sitter grammars to latest master"
```

---

## Build integration

`vendor/CMakeLists.txt` is included by the top-level `CMakeLists.txt` only
when `BATBOX_SYNTAX=ON` (the default):

```cmake
if(BATBOX_SYNTAX)
  add_subdirectory(vendor)
endif()
```

The output target is `batbox_treesitter` — a single static library containing
the tree-sitter core amalgamation plus all 10 grammar parsers.  Downstream
targets link it and receive the public `tree-sitter.h` header automatically
via CMake target interface:

```cmake
target_link_libraries(batbox_tui PRIVATE batbox_treesitter)
```

When `BATBOX_SYNTAX=OFF`, the vendor directory is skipped entirely and
`batbox_treesitter` is never created.  The `SyntaxHighlight` component falls
back to a plain-text renderer in that case.

---

## Licenses

Each vendored grammar carries its own license (MIT for all grammars listed
above as of the pinned commits).  License files are preserved in each
submodule directory (`LICENSE` or `LICENSE.md`).  The tree-sitter core
library is MIT licensed (`vendor/tree-sitter/LICENSE`).
