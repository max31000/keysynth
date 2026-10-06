#pragma once
// Bounded lock-free single-producer / single-consumer queue.
// Storage is allocated once in the constructor (control thread); push/pop never allocate, never block.
// Capacity is rounded up to a power of two; one slot is NOT wasted (indices are free-running counters).

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>

namespace ks {

template <typename T>
class SpscQueue {
    static_assert(std::is_nothrow_copy_assignable_v<T> || std::is_nothrow_move_assignable_v<T>,
                  "SpscQueue elements must be nothrow assignable");

public:
    explicit SpscQueue(std::size_t capacity) {
        std::size_t c = 1;
        while (c < capacity) c <<= 1;
        capacity_ = c;
        mask_ = c - 1;
        buffer_ = std::make_unique<T[]>(c);
    }
    SpscQueue(const SpscQueue&) = delete;
    SpscQueue& operator=(const SpscQueue&) = delete;

    // Producer side. Returns false when full (the item is dropped by the caller's policy).
    bool push(const T& item) noexcept {
        const uint64_t w = write_.load(std::memory_order_relaxed);
        if (w - readCache_ >= capacity_) {
            readCache_ = read_.load(std::memory_order_acquire);
            if (w - readCache_ >= capacity_) return false;
        }
        buffer_[static_cast<std::size_t>(w) & mask_] = item;
        write_.store(w + 1, std::memory_order_release);
        return true;
    }

    // Consumer side. Returns false when empty.
    bool pop(T& out) noexcept {
        const uint64_t r = read_.load(std::memory_order_relaxed);
        if (r == writeCache_) {
            writeCache_ = write_.load(std::memory_order_acquire);
            if (r == writeCache_) return false;
        }
        out = std::move(buffer_[static_cast<std::size_t>(r) & mask_]);
        read_.store(r + 1, std::memory_order_release);
        return true;
    }

    // Approximate (exact when called from either endpoint while the other is idle).
    std::size_t size() const noexcept {
        return static_cast<std::size_t>(write_.load(std::memory_order_acquire) -
                                        read_.load(std::memory_order_acquire));
    }
    bool empty() const noexcept { return size() == 0; }
    std::size_t capacity() const noexcept { return capacity_; }

private:
    std::unique_ptr<T[]> buffer_;
    std::size_t capacity_ = 0;
    std::size_t mask_ = 0;
    alignas(64) std::atomic<uint64_t> write_{0};
    uint64_t readCache_ = 0; // producer-owned
    alignas(64) std::atomic<uint64_t> read_{0};
    uint64_t writeCache_ = 0; // consumer-owned
};

} // namespace ks
