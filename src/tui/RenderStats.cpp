// src/tui/RenderStats.cpp
// =============================================================================
// RenderStats implementation.
//
// See include/batbox/tui/RenderStats.hpp for design notes.
// =============================================================================

#include "batbox/tui/RenderStats.hpp"

#include <algorithm>
#include <cstdlib>  // std::getenv

namespace batbox::tui {

// =============================================================================
// Singleton
// =============================================================================

RenderStats& RenderStats::global() noexcept {
    // Meyer's singleton — constructed once, never destroyed (intentional: the
    // render loop may call push_frame_now() from static-destructor phase of
    // other objects; keeping the instance alive avoids order-of-destruction bugs).
    static RenderStats instance;
    return instance;
}

// =============================================================================
// is_enabled()
// =============================================================================

bool RenderStats::is_enabled() noexcept {
    // Evaluated once per process.
    static const bool enabled = [] {
        const char* val = std::getenv("BATBOX_PERF");
        return val != nullptr && val[0] == '1';
    }();
    return enabled;
}

// =============================================================================
// push_frame_now()
// =============================================================================

void RenderStats::push_frame_now() noexcept {
    const int64_t ns = Clock::now().time_since_epoch().count();

    buf_[head_] = ns;
    head_ = (head_ + 1) % kCapacity;
    if (size_ < kCapacity) {
        ++size_;
    }
    // When size_ == kCapacity the ring is full; the oldest slot is implicitly
    // overwritten at head_ (which advanced past it).
}

// =============================================================================
// drain()
// =============================================================================

std::vector<RenderStats::TimePoint> RenderStats::drain() noexcept {
    // Reconstruct chronological order from the ring.
    // head_ points to the NEXT write slot.  The oldest valid entry is at:
    //   start = (size_ == kCapacity) ? head_ : 0
    // Elements run from start..head_-1 (wrapping).

    std::vector<TimePoint> result;
    result.reserve(size_);

    if (size_ == 0) {
        return result;
    }

    const std::size_t start = (size_ == kCapacity) ? head_ : 0;
    for (std::size_t i = 0; i < size_; ++i) {
        const std::size_t idx = (start + i) % kCapacity;
        result.emplace_back(
            std::chrono::nanoseconds{buf_[idx]});
    }

    // Reset after drain.
    size_ = 0;
    head_ = 0;

    return result;
}

// =============================================================================
// count()
// =============================================================================

std::size_t RenderStats::count() const noexcept {
    return size_;
}

} // namespace batbox::tui
