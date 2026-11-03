#include <doctest.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <stdexcept>
#include <thread>
#include <vector>

#include "audio_observer/spsc_ring_buffer.hpp"

using audio_observer::SpscRingBuffer;

namespace {

std::vector<float> ramp(float start, std::size_t count) {
    std::vector<float> values(count);
    for (std::size_t i = 0; i < count; ++i) {
        values[i] = start + static_cast<float>(i);
    }
    return values;
}

}  // namespace

TEST_CASE("capacity is exactly what was requested") {
    SpscRingBuffer<float> buffer(5);  // stored in 8 internally
    CHECK(buffer.capacity() == 5);
    CHECK(buffer.empty());
    CHECK(buffer.free_space() == 5);

    CHECK(buffer.push(ramp(0, 5).data(), 5));
    CHECK(buffer.size() == 5);
    CHECK(buffer.free_space() == 0);
    // A sixth element does not fit even though the storage has room.
    const float extra = 99.0f;
    CHECK_FALSE(buffer.push(&extra, 1));
    CHECK(buffer.overrun_count() == 1);
    CHECK(buffer.dropped_count() == 1);
}

TEST_CASE("zero capacity is rejected") {
    CHECK_THROWS_AS(SpscRingBuffer<float>(0), std::invalid_argument);
}

TEST_CASE("empty and full edges") {
    SpscRingBuffer<float> buffer(8);
    std::vector<float> out(16, -1.0f);

    // Popping from empty returns nothing and leaves the output alone.
    CHECK(buffer.pop(out.data(), out.size()) == 0);
    CHECK(out[0] == -1.0f);

    // Zero-length push always succeeds, even when full.
    CHECK(buffer.push(nullptr, 0));

    CHECK(buffer.push(ramp(0, 8).data(), 8));
    CHECK(buffer.size() == 8);
    CHECK(buffer.push(nullptr, 0));
    CHECK(buffer.overrun_count() == 0);

    // Pop with a smaller limit than available.
    CHECK(buffer.pop(out.data(), 3) == 3);
    CHECK(out[0] == 0.0f);
    CHECK(out[2] == 2.0f);
    CHECK(buffer.size() == 5);

    // Pop with a larger limit than available.
    CHECK(buffer.pop(out.data(), out.size()) == 5);
    CHECK(out[0] == 3.0f);
    CHECK(out[4] == 7.0f);
    CHECK(buffer.empty());
}

TEST_CASE("whole blocks wrap around the end of storage") {
    SpscRingBuffer<float> buffer(8);
    std::vector<float> out(8);

    float next_in = 0.0f;
    float next_out = 0.0f;
    // Blocks of 5 against storage of 8 straddle the end on most iterations.
    for (int round = 0; round < 100; ++round) {
        const auto block = ramp(next_in, 5);
        REQUIRE(buffer.push(block.data(), block.size()));
        next_in += 5.0f;
        const std::size_t n = buffer.pop(out.data(), out.size());
        REQUIRE(n == 5);
        for (std::size_t i = 0; i < n; ++i) {
            REQUIRE(out[i] == next_out);
            next_out += 1.0f;
        }
    }
    CHECK(buffer.overrun_count() == 0);
}

TEST_CASE("partial reads wrap around") {
    SpscRingBuffer<float> buffer(4);
    std::vector<float> out(4);
    REQUIRE(buffer.push(ramp(0, 3).data(), 3));
    REQUIRE(buffer.pop(out.data(), 2) == 2);  // read index at 2
    REQUIRE(buffer.push(ramp(3, 3).data(), 3));  // writes 3, 4 (wrap) 5
    REQUIRE(buffer.size() == 4);
    REQUIRE(buffer.pop(out.data(), 4) == 4);
    CHECK(out == std::vector<float>{2, 3, 4, 5});
}

TEST_CASE("overruns drop whole blocks and are counted") {
    SpscRingBuffer<float> buffer(10);
    std::vector<float> out(10);

    CHECK(buffer.push(ramp(0, 6).data(), 6));
    CHECK_FALSE(buffer.push(ramp(6, 5).data(), 5));  // 6 + 5 > 10
    CHECK(buffer.size() == 6);                        // nothing partial written
    CHECK(buffer.push(ramp(6, 4).data(), 4));         // exactly fills
    CHECK_FALSE(buffer.push(ramp(10, 1).data(), 1));
    CHECK_FALSE(buffer.push(ramp(10, 50).data(), 50));  // larger than capacity

    CHECK(buffer.overrun_count() == 3);
    CHECK(buffer.dropped_count() == 5 + 1 + 50);

    REQUIRE(buffer.pop(out.data(), out.size()) == 10);
    CHECK(out == ramp(0, 10));
}

TEST_CASE("works with other trivially copyable element types") {
    SpscRingBuffer<std::int32_t> buffer(3);
    const std::int32_t in[] = {-1, 2, -3};
    std::int32_t out[3] = {};
    CHECK(buffer.push(in, 3));
    CHECK(buffer.pop(out, 3) == 3);
    CHECK(out[2] == -3);
}

// Sequence values stay below 2^24 so every one is exact as a float.
constexpr std::size_t kStressSamples = 8'000'000;

TEST_CASE("two-thread stress: every sample arrives once, in order") {
    SpscRingBuffer<float> buffer(4096);
    std::atomic<bool> producer_done{false};

    std::thread producer([&] {
        std::vector<float> block(512);
        std::size_t sent = 0;
        std::size_t size = 1;
        while (sent < kStressSamples) {
            // Vary the block size from 1 to 509 to exercise every offset.
            size = (size * 7 + 3) % 509 + 1;
            const std::size_t n = std::min(size, kStressSamples - sent);
            for (std::size_t i = 0; i < n; ++i) {
                block[i] = static_cast<float>(sent + i);
            }
            // Retry until it fits so the sequence is complete; overruns are
            // still counted on each refusal.
            while (!buffer.push(block.data(), n)) {
                std::this_thread::yield();
            }
            sent += n;
        }
        producer_done.store(true, std::memory_order_release);
    });

    std::vector<float> out(777);
    std::size_t received = 0;
    std::size_t mismatches = 0;
    while (true) {
        const bool done = producer_done.load(std::memory_order_acquire);
        const std::size_t n = buffer.pop(out.data(), out.size());
        for (std::size_t i = 0; i < n; ++i) {
            if (out[i] != static_cast<float>(received + i)) {
                ++mismatches;
            }
        }
        received += n;
        if (n == 0) {
            if (done) {
                break;
            }
            std::this_thread::yield();
        }
    }
    producer.join();

    CHECK(mismatches == 0);
    CHECK(received == kStressSamples);
    CHECK(buffer.empty());
}

TEST_CASE("two-thread stress: drops are whole blocks and fully accounted for") {
    SpscRingBuffer<float> buffer(1024);
    std::atomic<bool> producer_done{false};

    std::thread producer([&] {
        std::vector<float> block(256);
        std::size_t sent = 0;
        while (sent < kStressSamples) {
            const std::size_t n = std::min<std::size_t>(256, kStressSamples - sent);
            for (std::size_t i = 0; i < n; ++i) {
                block[i] = static_cast<float>(sent + i);
            }
            buffer.push(block.data(), n);  // never waits, like an audio callback
            sent += n;
        }
        producer_done.store(true, std::memory_order_release);
    });

    std::vector<float> out(300);
    std::size_t received = 0;
    std::size_t order_errors = 0;
    std::size_t block_errors = 0;
    float last = -1.0f;
    while (true) {
        const bool done = producer_done.load(std::memory_order_acquire);
        const std::size_t n = buffer.pop(out.data(), out.size());
        for (std::size_t i = 0; i < n; ++i) {
            const float v = out[i];
            if (!(v > last)) {
                ++order_errors;
            }
            // A jump must land on a block boundary: only whole blocks drop.
            if (v != last + 1.0f && static_cast<std::size_t>(v) % 256 != 0) {
                ++block_errors;
            }
            last = v;
        }
        received += n;
        if (n == 0) {
            if (done) {
                break;
            }
            std::this_thread::yield();
        }
    }
    producer.join();

    CHECK(order_errors == 0);
    CHECK(block_errors == 0);
    CHECK(received + buffer.dropped_count() == kStressSamples);
    CHECK(buffer.dropped_count() == buffer.overrun_count() * 256);
}
