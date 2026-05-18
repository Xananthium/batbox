# cmake

CMake helper modules for batbox: dependency resolution, toolchain setup, sanitizers, egress checking, and build-time theme/skill embedding.

## Files

### Dependencies.cmake
vcpkg package resolution and target linking.

- `Dependencies.cmake` — finds vcpkg-installed packages (cpr, simdjson, spdlog, fmt, ftxui, ixwebsocket, cli11, doctest, nlohmann-json, zlib), exports them as interface targets

### VcpkgToolchain.cmake
vcpkg toolchain injection.

- `VcpkgToolchain.cmake` — bootstraps vcpkg.cmake toolchain file, sets VCPKG_ROOT and CMAKE_TOOLCHAIN_FILE, aborts with FATAL_ERROR if vcpkg is absent

### Sanitizers.cmake
ASan/UBSan/TSan compiler flag helpers.

- `Sanitizers.cmake` — appends -fsanitize= flags for address, undefined-behavior, or thread sanitizers based on BATBOX_SANITIZE cache variable; no-op in Release builds

### EgressCheck.cmake
Network egress policy checker.

- `EgressCheck.cmake` — invokes egress_scan.sh on source directories, fails the build if any compiled-in host other than configured allowlist is found

### egress_scan.sh
Shell script that greps for hardcoded hostnames/IPs in source.

- `egress_scan.sh` — scans all .cpp/.hpp for string literals containing network hostnames; exits non-zero when unapproved hosts are found

### Theme.cmake
Theme data embedding at configure time.

- `Theme.cmake` — reads data/themes.json and generates a C++ header (themes_data.hpp) with the JSON blob as a constexpr string literal, included by src/theme/

### DoctestAddTestsPatched.cmake
Patched version of doctest CTest discovery.

- `DoctestAddTestsPatched.cmake` — registers each doctest TEST_CASE as an individual CTest test; patched to handle batbox test binary argument format

### DoctestDiscoverPatched.cmake
Doctest test discovery module.

- `DoctestDiscoverPatched.cmake` — helper module called by DoctestAddTestsPatched.cmake; parses doctest --list-test-cases output into CTest test names
