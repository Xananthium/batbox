// =============================================================================
// src/core/Uuid.cpp — RFC 4122 Version 4 UUID implementation
// =============================================================================
//
// Entropy strategy (ordered by preference):
//   1. arc4random_buf()  — macOS / BSD: userspace CSPRNG, fork-safe
//   2. getrandom(2)      — Linux ≥ 3.17: kernel CSPRNG, no file descriptor
//   3. /dev/urandom      — POSIX fallback: never blocks, cryptographically safe
//
// std::random_device is intentionally NOT used; on some platforms it is
// deterministic (e.g. older MinGW, MSVC debug CRT) and insufficient for UUIDs.
//
// After random fill, two bit-fields are fixed per RFC 4122 §4.4:
//   byte[6] bits [7:4] = 0100  → version 4
//   byte[8] bits [7:6] = 10    → variant 1 (RFC 4122 / ITU-T X.667)
//
// FUTURE (CPP 0.1 merge):
//   Change parse() signature from  std::optional<Uuid>
//                              to  Result<Uuid, Error>
//   Replace `return std::nullopt` with `return Err(Error::invalid_argument(…))`
//   and replace `return uuid` with `return Ok(uuid)`.
//   The parse_impl() helper below contains all actual logic; only the wrapper
//   function changes.
//
// =============================================================================

#include "batbox/core/Uuid.hpp"

#include <array>
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

// ---------------------------------------------------------------------------
// Platform detection
// ---------------------------------------------------------------------------
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
#  define BATBOX_HAS_ARC4RANDOM
#  include <stdlib.h>   // arc4random_buf
#elif defined(__linux__)
#  define BATBOX_HAS_GETRANDOM
#  include <sys/random.h>
#endif

// ---------------------------------------------------------------------------
// Hex lookup tables
// ---------------------------------------------------------------------------
namespace {

// Lowercase hex digits for fast formatting
constexpr char kHexChars[] = "0123456789abcdef";

// Fill `buf` (16 bytes) with cryptographically random data.
// Throws std::runtime_error only if all entropy sources fail (catastrophic).
void fill_random(std::array<std::uint8_t, 16>& buf) {
#if defined(BATBOX_HAS_ARC4RANDOM)
    // macOS / BSD: arc4random_buf() never fails and never needs seeding.
    arc4random_buf(buf.data(), buf.size());

#elif defined(BATBOX_HAS_GETRANDOM)
    // Linux ≥ 3.17: getrandom() reads from the kernel CSPRNG.
    // Flags=0: block until the CSPRNG is seeded (safe at startup).
    ssize_t got = getrandom(buf.data(), buf.size(), 0);
    if (got == static_cast<ssize_t>(buf.size())) {
        return;
    }
    // Unexpected short read or error — fall through to the /dev/urandom block
    // below. (No [[fallthrough]] attribute: this is plain control flow, not a
    // switch case; on Linux BATBOX_HAS_ARC4RANDOM is undefined so the next
    // #if block is compiled in and execution continues into it.)

#endif

#if !defined(BATBOX_HAS_ARC4RANDOM)
    // /dev/urandom fallback: works on every POSIX system; never blocks.
    FILE* f = std::fopen("/dev/urandom", "rb");
    if (!f) {
        throw std::runtime_error("batbox::Uuid::v4(): cannot open /dev/urandom");
    }
    // NB: distinct name from the getrandom() `got` above — on Linux without
    // arc4random BOTH blocks compile into this one scope, so reusing `got` is a
    // redeclaration (conflicting ssize_t vs size_t).  Incidental Linux-build fix.
    std::size_t n_read = std::fread(buf.data(), 1, buf.size(), f);
    std::fclose(f);
    if (n_read != buf.size()) {
        throw std::runtime_error("batbox::Uuid::v4(): short read from /dev/urandom");
    }
#endif
}

// Decode a single lowercase hex nibble.  Returns -1 on invalid input.
constexpr int hex_nibble(char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Decode two hex characters into one byte.  Returns -1 if either char invalid.
constexpr int hex_byte(char hi, char lo) noexcept {
    int h = hex_nibble(hi);
    int l = hex_nibble(lo);
    if (h < 0 || l < 0) return -1;
    return (h << 4) | l;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// batbox::Uuid implementation
// ---------------------------------------------------------------------------
namespace batbox {

// Factory — generate a cryptographically random RFC 4122 v4 UUID
Uuid Uuid::v4() {
    Uuid u;
    fill_random(u.bytes);

    // RFC 4122 §4.4 — set version to 4
    //   byte[6]: clear bits [7:4], set to 0100 (0x4_)
    u.bytes[6] = (u.bytes[6] & 0x0Fu) | 0x40u;

    // RFC 4122 §4.4 — set variant to 0b10xxxxxx
    //   byte[8]: clear bits [7:6], set to 10 (0b10xx xxxx)
    u.bytes[8] = (u.bytes[8] & 0x3Fu) | 0x80u;

    return u;
}

// Parse canonical UUID string of the form:
//   xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx  (exactly 36 characters)
//
// Accepts both upper- and lower-case hex.
// Returns std::nullopt on any format error.
//
// FUTURE: When Result.hpp (CPP 0.1) is merged, change the signature to:
//   Result<Uuid, Error> parse(std::string_view sv)
// and replace `return std::nullopt` with `return Err(Error::invalid_argument(…))`
// and `return uuid` with `return Ok(uuid)`.
std::optional<Uuid> Uuid::parse(std::string_view sv) {
    // Must be exactly 36 characters.
    if (sv.size() != 36) return std::nullopt;

    // Dashes at positions 8, 13, 18, 23.
    if (sv[8] != '-' || sv[13] != '-' || sv[18] != '-' || sv[23] != '-') {
        return std::nullopt;
    }

    // Hex segments: 8-4-4-4-12 = 32 hex digits
    // Layout in sv:  [0..7] '-' [9..12] '-' [14..17] '-' [19..22] '-' [24..35]
    constexpr std::array<int, 16> positions = {
        // time_low (4 bytes, positions 0-7)
        0, 2, 4, 6,
        // time_mid (2 bytes, positions 9-12)
        9, 11,
        // time_hi_and_version (2 bytes, positions 14-17)
        14, 16,
        // clock_seq_hi_and_reserved + clock_seq_low (2 bytes, positions 19-22)
        19, 21,
        // node (6 bytes, positions 24-35)
        24, 26, 28, 30, 32, 34
    };

    Uuid uuid;
    for (int i = 0; i < 16; ++i) {
        int pos = positions[i];
        int b = hex_byte(sv[pos], sv[pos + 1]);
        if (b < 0) return std::nullopt;
        uuid.bytes[i] = static_cast<std::uint8_t>(b);
    }

    return uuid;
}

// Serialise to 36-character canonical lowercase string.
// Format: xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
std::string Uuid::to_string() const {
    // Pre-allocate exactly 36 characters.
    std::string s(36, '-');

    // Write hex digits into each byte position:
    //   byte indices → string positions
    //   [0..3]   →  [0..7]   (time_low)
    //   [4..5]   →  [9..12]  (time_mid)
    //   [6..7]   →  [14..17] (time_hi_and_version)
    //   [8..9]   →  [19..22] (clock_seq_hi_res + clock_seq_low)
    //   [10..15] →  [24..35] (node)
    constexpr std::array<int, 16> positions = {
        0, 2, 4, 6,
        9, 11,
        14, 16,
        19, 21,
        24, 26, 28, 30, 32, 34
    };

    for (int i = 0; i < 16; ++i) {
        uint8_t b = bytes[i];
        int pos = positions[i];
        s[pos]     = kHexChars[b >> 4];
        s[pos + 1] = kHexChars[b & 0x0Fu];
    }

    return s;
}

bool Uuid::is_nil() const noexcept {
    for (auto b : bytes) {
        if (b != 0) return false;
    }
    return true;
}

bool Uuid::operator<(const Uuid& other) const noexcept {
    return bytes < other.bytes;
}

} // namespace batbox

// ---------------------------------------------------------------------------
// std::hash<batbox::Uuid>
// ---------------------------------------------------------------------------
std::size_t std::hash<batbox::Uuid>::operator()(const batbox::Uuid& u) const noexcept {
    // FNV-1a over the 16 raw bytes — portable, collision-resistant for UUIDs.
    std::size_t h = 0xcbf29ce484222325ULL;
    for (auto b : u.bytes) {
        h ^= static_cast<std::size_t>(b);
        h *= 0x100000001b3ULL;
    }
    return h;
}
