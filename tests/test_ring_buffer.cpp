#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "marketscan/core/event.hpp"
#include "marketscan/core/ring_buffer.hpp"

using marketscan::MarketEvent;
using marketscan::SpscRingBuffer;

TEST_CASE("capacity is rounded up to a power of two", "[ring_buffer]") {
    REQUIRE(SpscRingBuffer<int>(1).capacity() == 2);
    REQUIRE(SpscRingBuffer<int>(2).capacity() == 2);
    REQUIRE(SpscRingBuffer<int>(3).capacity() == 4);
    REQUIRE(SpscRingBuffer<int>(1000).capacity() == 1024);
    REQUIRE(SpscRingBuffer<int>(1024).capacity() == 1024);
}

TEST_CASE("items come out in the order they went in", "[ring_buffer]") {
    SpscRingBuffer<int> q(8);
    REQUIRE(q.empty_approx());

    for (int i = 0; i < 5; ++i) {
        REQUIRE(q.try_push(i));
    }
    REQUIRE(q.size_approx() == 5);

    for (int i = 0; i < 5; ++i) {
        int out = -1;
        REQUIRE(q.try_pop(out));
        REQUIRE(out == i);
    }
    REQUIRE(q.empty_approx());

    int drained = -1;
    REQUIRE_FALSE(q.try_pop(drained));
}

// Every slot is usable: a full buffer is distinguished from an empty one by
// the monotonic indices, not by sacrificing a slot.
TEST_CASE("the whole capacity is usable", "[ring_buffer]") {
    SpscRingBuffer<int> q(4);
    REQUIRE(q.capacity() == 4);

    for (int i = 0; i < 4; ++i) {
        REQUIRE(q.try_push(i));
    }
    REQUIRE(q.size_approx() == 4);
    REQUIRE_FALSE(q.try_push(99));

    int out = -1;
    REQUIRE(q.try_pop(out));
    REQUIRE(out == 0);
    REQUIRE(q.try_push(99));
    REQUIRE_FALSE(q.try_push(100));
}

TEST_CASE("indices wrap without losing anything", "[ring_buffer]") {
    SpscRingBuffer<int> q(4);

    // Many more cycles than the capacity, so the masked indices wrap
    // repeatedly while the counters keep climbing.
    for (int i = 0; i < 10'000; ++i) {
        REQUIRE(q.try_push(i));
        int out = -1;
        REQUIRE(q.try_pop(out));
        REQUIRE(out == i);
    }
    REQUIRE(q.empty_approx());
}

TEST_CASE("carries MarketEvent, which is what it exists for", "[ring_buffer]") {
    SpscRingBuffer<MarketEvent> q(16);

    MS_Bar bar{};
    bar.ts = 1'700'000'000'000'000'000LL;
    bar.symbol = 7;
    bar.close = 123.45;
    REQUIRE(q.try_push(MarketEvent::from(bar)));

    MarketEvent out;
    REQUIRE(q.try_pop(out));
    REQUIRE(out.type == marketscan::EventType::Bar);
    REQUIRE(out.ts() == bar.ts);
    REQUIRE(out.symbol() == 7);
    REQUIRE(out.bar.close == 123.45);
}

// The structure's actual job. A feed thread fills it while the engine thread
// drains it; nothing may be lost, duplicated or reordered.
TEST_CASE("survives a real producer and consumer", "[ring_buffer][threads]") {
    constexpr int kCount = 200'000;
    SpscRingBuffer<std::uint64_t> q(1024);

    std::vector<std::uint64_t> received;
    received.reserve(kCount);

    std::thread producer([&] {
        for (std::uint64_t i = 0; i < kCount; ++i) {
            while (!q.try_push(i)) {
                std::this_thread::yield();
            }
        }
    });

    for (int i = 0; i < kCount; ++i) {
        std::uint64_t value = 0;
        while (!q.try_pop(value)) {
            std::this_thread::yield();
        }
        received.push_back(value);
    }
    producer.join();

    REQUIRE(received.size() == static_cast<std::size_t>(kCount));
    bool in_order = true;
    for (std::uint64_t i = 0; i < kCount; ++i) {
        if (received[static_cast<std::size_t>(i)] != i) {
            in_order = false;
            break;
        }
    }
    REQUIRE(in_order);
    REQUIRE(q.empty_approx());
}

// Producer and consumer state must not share a cache line. This is a
// performance property, but it is also the one thing about this class that is
// easy to break accidentally during a refactor, so it is pinned down here.
TEST_CASE("producer and consumer state sit on separate cache lines", "[ring_buffer]") {
    STATIC_REQUIRE(alignof(SpscRingBuffer<int>) >= marketscan::kCacheLineSize);
    REQUIRE(sizeof(SpscRingBuffer<int>) >= 2 * marketscan::kCacheLineSize);
}
