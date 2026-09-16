#ifndef MARKETSCAN_CORE_RING_BUFFER_HPP
#define MARKETSCAN_CORE_RING_BUFFER_HPP

#include <atomic>
#include <bit>
#include <cstddef>
#include <memory>
#include <new>
#include <stdexcept>
#include <type_traits>

namespace marketscan {

/// Assumed cache line size.
///
/// Not std::hardware_destructive_interference_size: MSVC warns that its value
/// is baked into the ABI of anything that uses it, and 64 is correct on every
/// x86-64 part this runs on. Getting it wrong costs throughput, not
/// correctness.
inline constexpr std::size_t kCacheLineSize = 64;

/// Single-producer, single-consumer lock-free queue.
///
/// This is the seam between a live feed thread and the engine thread. In replay
/// there is one thread and the engine reads its source directly, so this is not
/// on the replay path at all -- but the engine body either side of it is the
/// same code, which is the point.
///
/// Storage is allocated once, at construction. The hot path never allocates,
/// never locks and never makes a syscall.
///
/// The producer and consumer indices sit on separate cache lines, each next to
/// the cached copy of the opposite index that only its own thread touches.
/// Without that padding the two threads fight over one line and every push
/// invalidates the consumer's read, which costs more than the lock this
/// structure exists to avoid.
///
/// Indices count monotonically and are masked on use, so a full buffer is
/// distinguishable from an empty one without wasting a slot. They are
/// std::size_t and wrap correctly by unsigned arithmetic.
template<typename T>
class SpscRingBuffer {
    static_assert(std::is_trivially_copyable_v<T>,
                  "SpscRingBuffer holds trivially copyable payloads only");
    static_assert(std::is_default_constructible_v<T>);

public:
    /// `capacity` is rounded up to a power of two so the mask replaces a
    /// division. Must be at least 2.
    explicit SpscRingBuffer(std::size_t capacity)
        : capacity_(std::bit_ceil(capacity < 2 ? std::size_t{2} : capacity)),
          mask_(capacity_ - 1),
          slots_(std::make_unique<T[]>(capacity_)) {
        if (capacity_ == 0 || !std::has_single_bit(capacity_)) {
            throw std::invalid_argument("SpscRingBuffer capacity overflowed");
        }
    }

    SpscRingBuffer(const SpscRingBuffer&) = delete;
    SpscRingBuffer& operator=(const SpscRingBuffer&) = delete;
    SpscRingBuffer(SpscRingBuffer&&) = delete;
    SpscRingBuffer& operator=(SpscRingBuffer&&) = delete;
    ~SpscRingBuffer() = default;

    /// Producer thread only. Returns false when full.
    [[nodiscard]] bool try_push(const T& value) noexcept {
        const std::size_t tail = producer_.tail.load(std::memory_order_relaxed);

        // Re-read the consumer's index only when the cached copy says full.
        // That keeps the shared line out of the common case entirely.
        if (tail - producer_.cached_head >= capacity_) {
            producer_.cached_head = consumer_.head.load(std::memory_order_acquire);
            if (tail - producer_.cached_head >= capacity_) {
                return false;
            }
        }

        slots_[tail & mask_] = value;
        // Release: the slot write above must be visible before the consumer
        // can see the index that exposes it.
        producer_.tail.store(tail + 1, std::memory_order_release);
        return true;
    }

    /// Consumer thread only. Returns false when empty.
    [[nodiscard]] bool try_pop(T& out) noexcept {
        const std::size_t head = consumer_.head.load(std::memory_order_relaxed);

        if (head == consumer_.cached_tail) {
            consumer_.cached_tail = producer_.tail.load(std::memory_order_acquire);
            if (head == consumer_.cached_tail) {
                return false;
            }
        }

        out = slots_[head & mask_];
        consumer_.head.store(head + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

    /// Approximate: the other thread may have moved by the time this returns.
    /// For instrumentation and back-pressure heuristics, never for control
    /// flow that has to be exact.
    [[nodiscard]] std::size_t size_approx() const noexcept {
        const std::size_t tail = producer_.tail.load(std::memory_order_acquire);
        const std::size_t head = consumer_.head.load(std::memory_order_acquire);
        return tail - head;
    }

    [[nodiscard]] bool empty_approx() const noexcept { return size_approx() == 0; }

private:
    // C4324 fires on every struct below: MSVC is reporting that the alignment
    // specifier inserted padding. That padding is the entire point -- it is
    // what keeps the producer's and consumer's indices off each other's cache
    // line -- so the warning is acknowledged here rather than project-wide.
#if defined(_MSC_VER)
#    pragma warning(push)
#    pragma warning(disable : 4324)
#endif

    struct alignas(kCacheLineSize) ProducerState {
        std::atomic<std::size_t> tail{0};
        std::size_t cached_head{0};  // producer-private view of consumer_.head
    };

    struct alignas(kCacheLineSize) ConsumerState {
        std::atomic<std::size_t> head{0};
        std::size_t cached_tail{0};  // consumer-private view of producer_.tail
    };

    static_assert(std::atomic<std::size_t>::is_always_lock_free,
                  "SpscRingBuffer requires lock-free size_t atomics");

    const std::size_t capacity_;
    const std::size_t mask_;
    std::unique_ptr<T[]> slots_;

    alignas(kCacheLineSize) ProducerState producer_;
    alignas(kCacheLineSize) ConsumerState consumer_;

#if defined(_MSC_VER)
#    pragma warning(pop)
#endif
};

}  // namespace marketscan

#endif  // MARKETSCAN_CORE_RING_BUFFER_HPP
