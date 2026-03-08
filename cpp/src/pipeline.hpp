#pragma once

#include "parser.hpp"
#include "types.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <thread>

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

enum class MetricsEventType : uint8_t {
    MESSAGE,
    BOOK,
    PRICE_CHANGE,
    BEST_BID_ASK,
    TRADE
};

struct MetricsEvent {
    MetricsEventType type = MetricsEventType::MESSAGE;
    uint32_t msg_bytes = 0;
    float    parse_us = 0.0f;
    float    book_us = 0.0f;
    float    arb_us = 0.0f;
    float    e2e_us = 0.0f;
    bool     arb_checked = false;
};

struct alignas(64) MessageSlot {
    size_t   len = 0;
    NanoTime recv_time = 0;
    alignas(64) char data[MessageParser::kBufferCapacity + simdjson::SIMDJSON_PADDING];
};

template <typename T, size_t Capacity>
class SpscRing {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");

public:
    template <typename Writer>
    bool try_push(Writer&& writer) {
        const size_t head = head_.load(std::memory_order_relaxed);
        const size_t next = head + 1;
        if (next - tail_.load(std::memory_order_acquire) > Capacity) {
            return false;
        }

        writer(slots_[head & kMask]);
        head_.store(next, std::memory_order_release);
        return true;
    }

    bool try_push_copy(const T& value) {
        return try_push([&](T& slot) { slot = value; });
    }

    bool front(const T*& item) const {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire)) {
            return false;
        }
        item = &slots_[tail & kMask];
        return true;
    }

    bool front(T*& item) {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire)) {
            return false;
        }
        item = &slots_[tail & kMask];
        return true;
    }

    void pop() {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        tail_.store(tail + 1, std::memory_order_release);
    }

    bool empty() const {
        return tail_.load(std::memory_order_acquire) == head_.load(std::memory_order_acquire);
    }

    size_t size() const {
        const size_t head = head_.load(std::memory_order_acquire);
        const size_t tail = tail_.load(std::memory_order_acquire);
        return head - tail;
    }

private:
    static constexpr size_t kMask = Capacity - 1;

    alignas(64) std::array<T, Capacity> slots_{};
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};
};

inline void store_message_slot(MessageSlot& slot, const char* data, size_t len, NanoTime recv_time) {
    slot.len = len;
    slot.recv_time = recv_time;
    std::memcpy(slot.data, data, len);
    std::memset(slot.data + len, 0, simdjson::SIMDJSON_PADDING);
}

inline void cpu_relax() noexcept {
#if defined(__x86_64__) || defined(__i386__)
    _mm_pause();
#elif defined(__aarch64__)
    asm volatile("yield");
#else
    std::this_thread::yield();
#endif
}
