#pragma once

/// @file
/// Lock-free single-producer, single-consumer ring buffer.

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <type_traits>

namespace audio_observer {

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324)  // structure padded due to alignment specifier
#endif

/// Fixed-capacity ring buffer for one producer thread (for example an audio
/// callback) and one consumer thread.
///
/// Storage is allocated once in the constructor. push() and pop() never
/// allocate, lock, block or throw. The producer writes whole blocks or none:
/// a block that does not fit is dropped and counted as an overrun. The
/// consumer reads whatever is available.
///
/// Internally the storage size is the next power of two at or above the
/// requested capacity, but at most capacity() elements are ever held.
template <typename T>
class SpscRingBuffer {
    static_assert(std::is_trivially_copyable<T>::value,
                  "SpscRingBuffer elements must be trivially copyable");

public:
    /// Creates a buffer holding up to @p capacity elements.
    /// Throws std::invalid_argument if @p capacity is zero or too large.
    explicit SpscRingBuffer(std::size_t capacity)
        : capacity_(capacity), mask_(storage_size_for(capacity) - 1),
          storage_(new T[mask_ + 1]) {}

    SpscRingBuffer(const SpscRingBuffer&) = delete;
    SpscRingBuffer& operator=(const SpscRingBuffer&) = delete;

    /// Maximum number of elements held at once.
    std::size_t capacity() const noexcept { return capacity_; }

    /// Producer: appends all @p count elements, or none if they do not fit.
    /// Returns false and counts an overrun when the block is dropped.
    bool push(const T* data, std::size_t count) noexcept {
        const std::size_t write = write_index_.load(std::memory_order_relaxed);
        const std::size_t read = read_index_.load(std::memory_order_acquire);
        const std::size_t used = write - read;
        if (count > capacity_ - used) {
            overrun_count_.fetch_add(1, std::memory_order_relaxed);
            dropped_count_.fetch_add(count, std::memory_order_relaxed);
            return false;
        }
        copy_in(write, data, count);
        write_index_.store(write + count, std::memory_order_release);
        return true;
    }

    /// Consumer: copies up to @p max_count available elements into @p out
    /// and returns how many were copied.
    std::size_t pop(T* out, std::size_t max_count) noexcept {
        const std::size_t read = read_index_.load(std::memory_order_relaxed);
        const std::size_t write = write_index_.load(std::memory_order_acquire);
        const std::size_t count = std::min(write - read, max_count);
        copy_out(read, out, count);
        read_index_.store(read + count, std::memory_order_release);
        return count;
    }

    /// Elements available to the consumer. Exact on the consumer thread; a
    /// snapshot elsewhere.
    std::size_t size() const noexcept {
        const std::size_t read = read_index_.load(std::memory_order_acquire);
        const std::size_t write = write_index_.load(std::memory_order_acquire);
        return write - read;
    }

    /// Free space for the producer. Exact on the producer thread; a snapshot
    /// elsewhere.
    std::size_t free_space() const noexcept { return capacity_ - size(); }

    bool empty() const noexcept { return size() == 0; }

    /// Number of push() calls that dropped their block.
    std::uint64_t overrun_count() const noexcept {
        return overrun_count_.load(std::memory_order_relaxed);
    }

    /// Total elements dropped by overruns.
    std::uint64_t dropped_count() const noexcept {
        return dropped_count_.load(std::memory_order_relaxed);
    }

private:
    static std::size_t storage_size_for(std::size_t capacity) {
        // Indices grow without bound and wrap modulo 2^N; differences stay
        // correct as long as the storage size is at most half the index range.
        constexpr std::size_t max_storage = (std::size_t{1} << (sizeof(std::size_t) * 8 - 2));
        if (capacity == 0 || capacity > max_storage) {
            throw std::invalid_argument("SpscRingBuffer capacity must be in [1, 2^(N-2)]");
        }
        std::size_t size = 1;
        while (size < capacity) {
            size <<= 1;
        }
        return size;
    }

    void copy_in(std::size_t index, const T* data, std::size_t count) noexcept {
        if (count == 0) {
            return;
        }
        const std::size_t start = index & mask_;
        const std::size_t first = std::min(count, mask_ + 1 - start);
        std::memcpy(storage_.get() + start, data, first * sizeof(T));
        if (first < count) {
            std::memcpy(storage_.get(), data + first, (count - first) * sizeof(T));
        }
    }

    void copy_out(std::size_t index, T* out, std::size_t count) const noexcept {
        if (count == 0) {
            return;
        }
        const std::size_t start = index & mask_;
        const std::size_t first = std::min(count, mask_ + 1 - start);
        std::memcpy(out, storage_.get() + start, first * sizeof(T));
        if (first < count) {
            std::memcpy(out + first, storage_.get(), (count - first) * sizeof(T));
        }
    }

    static constexpr std::size_t cache_line = 64;

    const std::size_t capacity_;
    const std::size_t mask_;
    const std::unique_ptr<T[]> storage_;

    // Producer-owned, consumer-read.
    alignas(cache_line) std::atomic<std::size_t> write_index_{0};
    std::atomic<std::uint64_t> overrun_count_{0};
    std::atomic<std::uint64_t> dropped_count_{0};
    // Consumer-owned, producer-read.
    alignas(cache_line) std::atomic<std::size_t> read_index_{0};
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

}  // namespace audio_observer
