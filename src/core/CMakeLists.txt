# src/core/CMakeLists.txt
# ---------------------------------------------------------------------------
# BatBox core utility library — logging, error types, path helpers.
#
# This target is intentionally declared as an OBJECT library so that all
# object files are compiled once and then linked into the final executable
# without an extra static archive step. Downstream targets link it with:
#
#   target_link_libraries(<target> PRIVATE batbox_core)
#
# NOTE: This subdirectory is wired into the top-level build in task CPP B.4.
#       Do NOT add add_subdirectory(src/core) to the root CMakeLists.txt here.
# ---------------------------------------------------------------------------

cmake_minimum_required(VERSION 3.24)

add_library(batbox_core OBJECT
  Paths.cpp
  Logging.cpp       # CPP 0.3 — spdlog rotating-file wrapper
  Uuid.cpp          # CPP 0.6
  CancelToken.cpp   # CPP 0.x — cancellation token primitives
  Json.cpp          # CPP 0.x — JSON helpers
)

# spdlog — used by Logging.cpp
find_package(spdlog CONFIG REQUIRED)

# simdjson — used by Json.cpp
find_package(simdjson CONFIG REQUIRED)

target_include_directories(batbox_core
  PUBLIC
    # Allow #include "batbox/core/Logging.hpp" (and siblings) from anywhere
    # that links this target.
    "${CMAKE_SOURCE_DIR}/include"
)

target_link_libraries(batbox_core
  PUBLIC
    spdlog::spdlog
    simdjson::simdjson
)

target_compile_features(batbox_core PUBLIC cxx_std_20)
