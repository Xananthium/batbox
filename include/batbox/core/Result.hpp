// include/batbox/core/Result.hpp
//
// batbox::Result<T,E> — Rust-style error-return type for every component.
//
// Strategy (auto-selected at compile time):
//   C++23 or later  → thin type alias over std::expected<T,E>; zero overhead,
//                      full monadic API (and_then / or_else / transform).
//   C++20           → polyfill that mirrors the std::expected API exactly so
//                      call-sites are identical on both language revisions.
//
// Constructing a Result:
//
//   // OK side — implicit from T:
//   Result<int> r = 42;
//
//   // Error side — always explicit, using batbox::Unexpected<E>:
//   Result<int, std::string> e = batbox::Unexpected("oops");
//
//   // Void OK:
//   Result<void, std::string> v;         // default = ok
//
// On the C++23 path batbox::Unexpected wraps std::unexpected so the same
// source compiles on both paths.

#pragma once

#include <cassert>
#include <functional>
#include <string>
#include <type_traits>
#include <utility>

// ---------------------------------------------------------------------------
// Detect C++23 std::expected availability.
// ---------------------------------------------------------------------------
#if defined(__cpp_lib_expected) && __cpp_lib_expected >= 202211L
#  define BATBOX_HAS_STD_EXPECTED 1
#else
#  define BATBOX_HAS_STD_EXPECTED 0
#endif

#if BATBOX_HAS_STD_EXPECTED
#  include <expected>
#endif

namespace batbox {

// ============================================================================
// Unexpected<E> — error-side tag, mirrors std::unexpected semantics.
// ============================================================================

#if BATBOX_HAS_STD_EXPECTED

/// On C++23 Unexpected<E> is a transparent alias for std::unexpected<E>.
template <typename E>
using Unexpected = std::unexpected<E>;

/// Factory: produce a std::unexpected from a value.
template <typename E>
[[nodiscard]] constexpr std::unexpected<std::decay_t<E>> make_unexpected(E&& e) {
    return std::unexpected<std::decay_t<E>>(std::forward<E>(e));
}

#else // C++20 polyfill

/// On C++20 Unexpected<E> is our own tag that carries the error value.
template <typename E>
struct Unexpected {
    using error_type = E;
    E value;

    template <typename F,
              typename = std::enable_if_t<std::is_constructible_v<E, F&&>>>
    constexpr explicit Unexpected(F&& e) : value(std::forward<F>(e)) {}

    [[nodiscard]] constexpr       E& error()       & noexcept { return value; }
    [[nodiscard]] constexpr const E& error() const & noexcept { return value; }
    [[nodiscard]] constexpr       E&& error()       && noexcept { return std::move(value); }
    [[nodiscard]] constexpr const E&& error() const && noexcept { return std::move(value); }
};

/// Factory mirroring std::unexpected construction.
template <typename E>
[[nodiscard]] constexpr Unexpected<std::decay_t<E>> make_unexpected(E&& e) {
    return Unexpected<std::decay_t<E>>(std::forward<E>(e));
}

#endif // BATBOX_HAS_STD_EXPECTED

// Convenience alias so call-sites can write batbox::Err("msg").
template <typename E>
[[nodiscard]] constexpr auto Err(E&& e) {
    return make_unexpected(std::forward<E>(e));
}

// ============================================================================
// C++23 path: thin alias over std::expected
// ============================================================================
#if BATBOX_HAS_STD_EXPECTED

/// batbox::Result<T,E> on C++23 is a direct alias for std::expected<T,E>.
template <typename T, typename E = std::string>
using Result = std::expected<T, E>;

// ============================================================================
// C++20 polyfill
// ============================================================================
#else

// Forward declarations
template <typename T, typename E = std::string> class Result;
template <typename E>                            class Result<void, E>;

// ---------------------------------------------------------------------------
// Primary template: Result<T, E>  (T != void)
// ---------------------------------------------------------------------------
template <typename T, typename E>
class [[nodiscard]] Result {
public:
    using value_type = T;
    using error_type = E;

    // ---- Constructors -------------------------------------------------------

    /// Construct ok from T (or anything T is constructible from).
    /// Uses SFINAE to avoid ambiguity with the Unexpected constructor.
    template <typename U = T,
              typename = std::enable_if_t<
                  std::is_constructible_v<T, U&&> &&
                  !std::is_same_v<std::decay_t<U>, Result> &&
                  !std::is_same_v<std::decay_t<U>, Unexpected<E>>>>
    constexpr /*implicit*/ Result(U&& v)  // NOLINT(google-explicit-constructor)
        : ok_(true) {
        ::new (buf()) T(std::forward<U>(v));
    }

    /// Construct error from Unexpected<E>.
    template <typename F,
              typename = std::enable_if_t<std::is_constructible_v<E, F&&>>>
    constexpr /*implicit*/ Result(Unexpected<F> u)  // NOLINT(google-explicit-constructor)
        : ok_(false) {
        ::new (buf()) E(std::move(u.value));
    }

    // Copy constructor
    constexpr Result(const Result& o) : ok_(o.ok_) {
        if (ok_) ::new (buf()) T(*o.ok_ptr());
        else     ::new (buf()) E(*o.err_ptr());
    }

    // Move constructor
    constexpr Result(Result&& o) noexcept(
        std::is_nothrow_move_constructible_v<T> &&
        std::is_nothrow_move_constructible_v<E>)
        : ok_(o.ok_) {
        if (ok_) ::new (buf()) T(std::move(*o.ok_ptr()));
        else     ::new (buf()) E(std::move(*o.err_ptr()));
    }

    // Destructor
    ~Result() { destroy(); }

    // Copy assignment
    Result& operator=(const Result& o) {
        if (this == &o) return *this;
        destroy();
        ok_ = o.ok_;
        if (ok_) ::new (buf()) T(*o.ok_ptr());
        else     ::new (buf()) E(*o.err_ptr());
        return *this;
    }

    // Move assignment
    Result& operator=(Result&& o) noexcept(
        std::is_nothrow_move_assignable_v<T> &&
        std::is_nothrow_move_assignable_v<E>) {
        if (this == &o) return *this;
        destroy();
        ok_ = o.ok_;
        if (ok_) ::new (buf()) T(std::move(*o.ok_ptr()));
        else     ::new (buf()) E(std::move(*o.err_ptr()));
        return *this;
    }

    // ---- Observers ----------------------------------------------------------

    [[nodiscard]] constexpr bool has_value() const noexcept { return ok_; }
    [[nodiscard]] constexpr explicit operator bool() const noexcept { return ok_; }

    [[nodiscard]] constexpr T& value() & {
        assert(ok_ && "Result::value() on error Result");
        return *ok_ptr();
    }
    [[nodiscard]] constexpr const T& value() const& {
        assert(ok_ && "Result::value() on error Result");
        return *ok_ptr();
    }
    [[nodiscard]] constexpr T&& value() && {
        assert(ok_ && "Result::value() on error Result");
        return std::move(*ok_ptr());
    }
    [[nodiscard]] constexpr const T&& value() const&& {
        assert(ok_ && "Result::value() on error Result");
        return std::move(*ok_ptr());
    }

    [[nodiscard]] constexpr E& error() & {
        assert(!ok_ && "Result::error() on ok Result");
        return *err_ptr();
    }
    [[nodiscard]] constexpr const E& error() const& {
        assert(!ok_ && "Result::error() on ok Result");
        return *err_ptr();
    }
    [[nodiscard]] constexpr E&& error() && {
        assert(!ok_ && "Result::error() on ok Result");
        return std::move(*err_ptr());
    }
    [[nodiscard]] constexpr const E&& error() const&& {
        assert(!ok_ && "Result::error() on ok Result");
        return std::move(*err_ptr());
    }

    template <typename U>
    [[nodiscard]] constexpr T value_or(U&& def) const& {
        return ok_ ? *ok_ptr() : static_cast<T>(std::forward<U>(def));
    }
    template <typename U>
    [[nodiscard]] constexpr T value_or(U&& def) && {
        return ok_ ? std::move(*ok_ptr()) : static_cast<T>(std::forward<U>(def));
    }

    // ---- Pointer-like access (ok side only) ---------------------------------

    [[nodiscard]] constexpr T* operator->() noexcept {
        assert(ok_); return ok_ptr();
    }
    [[nodiscard]] constexpr const T* operator->() const noexcept {
        assert(ok_); return ok_ptr();
    }
    [[nodiscard]] constexpr T& operator*() & noexcept {
        assert(ok_); return *ok_ptr();
    }
    [[nodiscard]] constexpr const T& operator*() const& noexcept {
        assert(ok_); return *ok_ptr();
    }
    [[nodiscard]] constexpr T&& operator*() && noexcept {
        assert(ok_); return std::move(*ok_ptr());
    }
    [[nodiscard]] constexpr const T&& operator*() const&& noexcept {
        assert(ok_); return std::move(*ok_ptr());
    }

    // ---- Monadic combinators ------------------------------------------------

    template <typename F>
    [[nodiscard]] constexpr auto and_then(F&& f) & {
        using Ret = std::invoke_result_t<F, T&>;
        if (ok_) return std::invoke(std::forward<F>(f), *ok_ptr());
        return Ret(Unexpected<E>(*err_ptr()));
    }
    template <typename F>
    [[nodiscard]] constexpr auto and_then(F&& f) const& {
        using Ret = std::invoke_result_t<F, const T&>;
        if (ok_) return std::invoke(std::forward<F>(f), *ok_ptr());
        return Ret(Unexpected<E>(*err_ptr()));
    }
    template <typename F>
    [[nodiscard]] constexpr auto and_then(F&& f) && {
        using Ret = std::invoke_result_t<F, T&&>;
        if (ok_) return std::invoke(std::forward<F>(f), std::move(*ok_ptr()));
        return Ret(Unexpected<E>(std::move(*err_ptr())));
    }

    template <typename F>
    [[nodiscard]] constexpr auto or_else(F&& f) & {
        using Ret = std::invoke_result_t<F, E&>;
        if (!ok_) return std::invoke(std::forward<F>(f), *err_ptr());
        return Ret(*ok_ptr());
    }
    template <typename F>
    [[nodiscard]] constexpr auto or_else(F&& f) const& {
        using Ret = std::invoke_result_t<F, const E&>;
        if (!ok_) return std::invoke(std::forward<F>(f), *err_ptr());
        return Ret(*ok_ptr());
    }
    template <typename F>
    [[nodiscard]] constexpr auto or_else(F&& f) && {
        using Ret = std::invoke_result_t<F, E&&>;
        if (!ok_) return std::invoke(std::forward<F>(f), std::move(*err_ptr()));
        return Ret(std::move(*ok_ptr()));
    }

    template <typename F>
    [[nodiscard]] constexpr auto transform(F&& f) & {
        using U = std::invoke_result_t<F, T&>;
        if (ok_) return Result<U, E>(std::invoke(std::forward<F>(f), *ok_ptr()));
        return Result<U, E>(Unexpected<E>(*err_ptr()));
    }
    template <typename F>
    [[nodiscard]] constexpr auto transform(F&& f) const& {
        using U = std::invoke_result_t<F, const T&>;
        if (ok_) return Result<U, E>(std::invoke(std::forward<F>(f), *ok_ptr()));
        return Result<U, E>(Unexpected<E>(*err_ptr()));
    }
    template <typename F>
    [[nodiscard]] constexpr auto transform(F&& f) && {
        using U = std::invoke_result_t<F, T&&>;
        if (ok_) return Result<U, E>(std::invoke(std::forward<F>(f), std::move(*ok_ptr())));
        return Result<U, E>(Unexpected<E>(std::move(*err_ptr())));
    }

    template <typename F>
    [[nodiscard]] constexpr auto transform_error(F&& f) & {
        using F2 = std::invoke_result_t<F, E&>;
        if (!ok_) return Result<T, F2>(Unexpected<F2>(std::invoke(std::forward<F>(f), *err_ptr())));
        return Result<T, F2>(*ok_ptr());
    }
    template <typename F>
    [[nodiscard]] constexpr auto transform_error(F&& f) const& {
        using F2 = std::invoke_result_t<F, const E&>;
        if (!ok_) return Result<T, F2>(Unexpected<F2>(std::invoke(std::forward<F>(f), *err_ptr())));
        return Result<T, F2>(*ok_ptr());
    }
    template <typename F>
    [[nodiscard]] constexpr auto transform_error(F&& f) && {
        using F2 = std::invoke_result_t<F, E&&>;
        if (!ok_) return Result<T, F2>(Unexpected<F2>(std::invoke(std::forward<F>(f), std::move(*err_ptr()))));
        return Result<T, F2>(std::move(*ok_ptr()));
    }

    // ---- Equality -----------------------------------------------------------
    template <typename T2, typename E2>
    [[nodiscard]] friend constexpr bool operator==(const Result& lhs,
                                                    const Result<T2,E2>& rhs) {
        if (lhs.ok_ != rhs.has_value()) return false;
        if (lhs.ok_) return lhs.value() == rhs.value();
        return lhs.error() == rhs.error();
    }

private:
    static constexpr std::size_t kSize  = sizeof(T) > sizeof(E) ? sizeof(T) : sizeof(E);
    static constexpr std::size_t kAlign = alignof(T) > alignof(E) ? alignof(T) : alignof(E);
    alignas(kAlign) unsigned char storage_[kSize];
    bool ok_;

    void*       buf()          noexcept { return storage_; }
    const void* buf()    const noexcept { return storage_; }
    T*          ok_ptr()       noexcept { return static_cast<T*>(buf()); }
    const T*    ok_ptr() const noexcept { return static_cast<const T*>(buf()); }
    E*          err_ptr()      noexcept { return static_cast<E*>(buf()); }
    const E*    err_ptr()const noexcept { return static_cast<const E*>(buf()); }

    void destroy() noexcept {
        if (ok_) ok_ptr()->~T();
        else     err_ptr()->~E();
    }
};

// ---------------------------------------------------------------------------
// Void specialization: Result<void, E>
// ---------------------------------------------------------------------------
template <typename E>
class [[nodiscard]] Result<void, E> {
public:
    using value_type = void;
    using error_type = E;

    /// Default construct = ok (no value, no error).
    constexpr Result() noexcept : ok_(true) {}

    /// Construct from Unexpected<E>.
    template <typename F,
              typename = std::enable_if_t<std::is_constructible_v<E, F&&>>>
    constexpr /*implicit*/ Result(Unexpected<F> u)  // NOLINT(google-explicit-constructor)
        : ok_(false) {
        ::new (buf()) E(std::move(u.value));
    }

    // Copy constructor
    constexpr Result(const Result& o) : ok_(o.ok_) {
        if (!ok_) ::new (buf()) E(*o.err_ptr());
    }

    // Move constructor
    constexpr Result(Result&& o) noexcept(std::is_nothrow_move_constructible_v<E>)
        : ok_(o.ok_) {
        if (!ok_) ::new (buf()) E(std::move(*o.err_ptr()));
    }

    ~Result() { destroy(); }

    Result& operator=(const Result& o) {
        if (this == &o) return *this;
        destroy();
        ok_ = o.ok_;
        if (!ok_) ::new (buf()) E(*o.err_ptr());
        return *this;
    }

    Result& operator=(Result&& o) noexcept(std::is_nothrow_move_assignable_v<E>) {
        if (this == &o) return *this;
        destroy();
        ok_ = o.ok_;
        if (!ok_) ::new (buf()) E(std::move(*o.err_ptr()));
        return *this;
    }

    [[nodiscard]] constexpr bool has_value() const noexcept { return ok_; }
    [[nodiscard]] constexpr explicit operator bool() const noexcept { return ok_; }

    /// value() on void Result just asserts it's ok; returns void.
    constexpr void value() const {
        assert(ok_ && "Result<void,E>::value() on error Result");
    }

    [[nodiscard]] constexpr E& error() & {
        assert(!ok_ && "Result<void,E>::error() on ok Result");
        return *err_ptr();
    }
    [[nodiscard]] constexpr const E& error() const& {
        assert(!ok_ && "Result<void,E>::error() on ok Result");
        return *err_ptr();
    }
    [[nodiscard]] constexpr E&& error() && {
        assert(!ok_ && "Result<void,E>::error() on ok Result");
        return std::move(*err_ptr());
    }
    [[nodiscard]] constexpr const E&& error() const&& {
        assert(!ok_ && "Result<void,E>::error() on ok Result");
        return std::move(*err_ptr());
    }

    template <typename F>
    [[nodiscard]] constexpr auto and_then(F&& f) & {
        using Ret = std::invoke_result_t<F>;
        if (ok_) return std::invoke(std::forward<F>(f));
        return Ret(Unexpected<E>(*err_ptr()));
    }
    template <typename F>
    [[nodiscard]] constexpr auto and_then(F&& f) const& {
        using Ret = std::invoke_result_t<F>;
        if (ok_) return std::invoke(std::forward<F>(f));
        return Ret(Unexpected<E>(*err_ptr()));
    }
    template <typename F>
    [[nodiscard]] constexpr auto and_then(F&& f) && {
        using Ret = std::invoke_result_t<F>;
        if (ok_) return std::invoke(std::forward<F>(f));
        return Ret(Unexpected<E>(std::move(*err_ptr())));
    }

    template <typename F>
    [[nodiscard]] constexpr auto or_else(F&& f) & {
        using Ret = std::invoke_result_t<F, E&>;
        if (!ok_) return std::invoke(std::forward<F>(f), *err_ptr());
        return Ret{};
    }
    template <typename F>
    [[nodiscard]] constexpr auto or_else(F&& f) const& {
        using Ret = std::invoke_result_t<F, const E&>;
        if (!ok_) return std::invoke(std::forward<F>(f), *err_ptr());
        return Ret{};
    }
    template <typename F>
    [[nodiscard]] constexpr auto or_else(F&& f) && {
        using Ret = std::invoke_result_t<F, E&&>;
        if (!ok_) return std::invoke(std::forward<F>(f), std::move(*err_ptr()));
        return Ret{};
    }

    template <typename F>
    [[nodiscard]] constexpr auto transform(F&& f) & {
        using U = std::invoke_result_t<F>;
        if (ok_) return Result<U, E>(std::invoke(std::forward<F>(f)));
        return Result<U, E>(Unexpected<E>(*err_ptr()));
    }
    template <typename F>
    [[nodiscard]] constexpr auto transform(F&& f) const& {
        using U = std::invoke_result_t<F>;
        if (ok_) return Result<U, E>(std::invoke(std::forward<F>(f)));
        return Result<U, E>(Unexpected<E>(*err_ptr()));
    }
    template <typename F>
    [[nodiscard]] constexpr auto transform(F&& f) && {
        using U = std::invoke_result_t<F>;
        if (ok_) return Result<U, E>(std::invoke(std::forward<F>(f)));
        return Result<U, E>(Unexpected<E>(std::move(*err_ptr())));
    }

    template <typename E2>
    [[nodiscard]] friend constexpr bool operator==(const Result& lhs,
                                                    const Result<void,E2>& rhs) {
        if (lhs.ok_ != rhs.has_value()) return false;
        if (lhs.ok_) return true;
        return lhs.error() == rhs.error();
    }

private:
    alignas(E) unsigned char storage_[sizeof(E)];
    bool ok_;

    void*       buf()          noexcept { return storage_; }
    const void* buf()    const noexcept { return storage_; }
    E*          err_ptr()      noexcept { return static_cast<E*>(buf()); }
    const E*    err_ptr() const noexcept { return static_cast<const E*>(buf()); }

    void destroy() noexcept { if (!ok_) err_ptr()->~E(); }
};

// Convenience helper aliases for the polyfill path.
template <typename T>
[[nodiscard]] constexpr T&& make_ok(T&& v) noexcept { return std::forward<T>(v); }

#endif // !BATBOX_HAS_STD_EXPECTED

} // namespace batbox
