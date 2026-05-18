# tests

Test infrastructure root: CMakeLists.txt registers unit and integration test binaries with CTest.

## Files

### CMakeLists.txt
Registers all test targets via DoctestAddTestsPatched; enables CTest; groups tests by label (unit, integration, tui-smoke).

### CMakeLists.txt (root tests)
Top-level test build configuration linking test binaries against the batbox static libraries.
