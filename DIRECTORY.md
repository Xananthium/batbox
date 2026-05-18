# . (repo root)

Top-level project files for batbox: the C++20 terminal AI assistant. Contains the main CMakeLists.txt, vcpkg manifest, and entry-point source.

## Files

### CMakeLists.txt
Root CMake build configuration.

- `CMakeLists.txt` — defines the batbox executable target, enables vcpkg, sets C++20 standard, includes cmake/ modules, and wires all src/ subdirectory targets

### vcpkg.json
vcpkg dependency manifest.

- `vcpkg.json` — declares pinned versions of cpr, simdjson, spdlog, fmt, ftxui, ixwebsocket, cli11, doctest, nlohmann-json, zlib, openssl
