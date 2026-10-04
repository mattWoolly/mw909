// SPDX-License-Identifier: AGPL-3.0-or-later
// mw909 — classic hybrid drum machine emulation. Copyright (C) 2026 mwAudio.
//
// Bounded single-producer / single-consumer queue of trivially copyable values.
// Wait-free on both sides: push and pop never allocate, lock or spin.

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <type_traits>

namespace mw909::util {

// Capacity must be a power of two. Holds up to Capacity - 1 items.
template <typename T, std::size_t Capacity>
class SpscQueue {
    static_assert(std::is_trivially_copyable_v<T>);
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");

public:
    // Producer thread only. Returns false when full (the item is not queued).
    bool push(const T& item) noexcept {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t next = (head + 1) & kMask;
        if (next == tail_.load(std::memory_order_acquire)) return false;
        slots_[head] = item;
        head_.store(next, std::memory_order_release);
        return true;
    }

    // Consumer thread only. Returns false when empty.
    bool pop(T& out) noexcept {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire)) return false;
        out = slots_[tail];
        tail_.store((tail + 1) & kMask, std::memory_order_release);
        return true;
    }

    // Approximate (exact only when the other side is idle).
    std::size_t sizeApprox() const noexcept {
        return (head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire)) & kMask;
    }

private:
    static constexpr std::size_t kMask = Capacity - 1;

    std::array<T, Capacity> slots_{};
    alignas(64) std::atomic<std::size_t> head_{0};
    alignas(64) std::atomic<std::size_t> tail_{0};
};

}  // namespace mw909::util
