#pragma once
// =============================================================================
// batbox/core/Uuid.hpp — RFC 4122 Version 4 UUID
// =============================================================================
//
// Provides:
//   struct batbox::Uuid         — 128-bit value type (16 bytes)
//   batbox::Uuid::v4()          — cryptographically random UUIDv4 factory
//   batbox::Uuid::parse(sv)     — parse canonical string → std::optional<Uuid>
//                                 NOTE: When CPP 0.1 (Result.hpp) is complete,
//                                 this signature will change to:
//                                   Result<Uuid, Error> parse(std::string_view)
//                                 The internal logic is identical; only the
//                                 return wrapper changes.  See src/core/Uuid.cpp
//                                 for the swap-in comment.
//   batbox::Uuid::to_string()   — canonical lowercase 36-char form
//   batbox::Uuid::is_nil()
//   operator==, operator<, std::hash<batbox::Uuid>
//
// Entropy source (in priority order):
//   1. arc4random_buf()    — macOS / BSD (CSPRNG, no syscall needed)
//   2. getrandom()         — Linux ≥ 3.17
//   3. /dev/urandom        — universal POSIX fallback
//
// std::random_device is NOT used for entropy.  On some platforms (MSVC debug
// CRT, older MinGW) it is deterministic and inadequate for unique IDs.
//
// Thread-safety:
//   v4() is thread-safe.  arc4random_buf() uses per-thread state.  The
//   getrandom/urandom paths perform one syscall per call; no shared state.
//
// =============================================================================

#include <array>
#include <cstdint>
#include <functional>   // std::hash
#include <optional>
#include <string>
#include <string_view>

namespace batbox {

// ---------------------------------------------------------------------------
// Uuid — 128-bit value type stored as 16 raw bytes (big-endian / network order)
// ---------------------------------------------------------------------------
struct Uuid {
    std::array<std::uint8_t, 16> bytes{};

    // -----------------------------------------------------------------------
    // Factory — generate a cryptographically random RFC 4122 v4 UUID
    // -----------------------------------------------------------------------
    [[nodiscard]] static Uuid v4();

    // -----------------------------------------------------------------------
    // Parse canonical form:  xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx  (36 chars)
    //
    // Returns std::nullopt on any format error.
    //
    // FUTURE (CPP 0.1 merge): change return type to Result<Uuid, Error> and
    // propagate an Error::invalid_argument when the format check fails.
    // The body of parse_impl() in Uuid.cpp is the shared implementation.
    // -----------------------------------------------------------------------
    [[nodiscard]] static std::optional<Uuid> parse(std::string_view sv);

    // -----------------------------------------------------------------------
    // Serialise — returns the 36-character canonical lowercase string
    //   e.g. "f47ac10b-58cc-4372-a567-0e02b2c3d479"
    // -----------------------------------------------------------------------
    [[nodiscard]] std::string to_string() const;

    // -----------------------------------------------------------------------
    // Nil UUID — all bytes zero
    // -----------------------------------------------------------------------
    [[nodiscard]] bool is_nil() const noexcept;

    [[nodiscard]] static constexpr Uuid nil() noexcept { return Uuid{}; }

    // -----------------------------------------------------------------------
    // Comparison
    // -----------------------------------------------------------------------
    [[nodiscard]] bool operator==(const Uuid& other) const noexcept = default;
    [[nodiscard]] bool operator< (const Uuid& other) const noexcept;
};

// ---------------------------------------------------------------------------
// uuid_v4() — convenience free function returning a new UUID string directly.
//
// Equivalent to Uuid::v4().to_string().  Provided so call sites that need a
// string UUID do not have to chain the static factory and to_string():
//
//   std::string id = batbox::uuid_v4();
//
// Thread-safe (delegates to Uuid::v4()).
// ---------------------------------------------------------------------------
[[nodiscard]] inline std::string uuid_v4() {
    return Uuid::v4().to_string();
}

} // namespace batbox

// ---------------------------------------------------------------------------
// std::hash<batbox::Uuid> — enables use in unordered_map / unordered_set
// ---------------------------------------------------------------------------
template <>
struct std::hash<batbox::Uuid> {
    [[nodiscard]] std::size_t operator()(const batbox::Uuid& u) const noexcept;
};
