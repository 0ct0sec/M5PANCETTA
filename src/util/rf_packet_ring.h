#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace RfPacketRing {

/**
 * Fixed-size single-producer/single-consumer ring.
 *
 * The Wi-Fi task is the only producer and the main loop is the only consumer.
 * Items are fully written before the producer publishes the head index. When
 * the consumer falls behind, the newest observation is rejected and the loss
 * is counted explicitly instead of overwriting older evidence.
 */
template <typename T, std::size_t Capacity>
class SpscRing {
    static_assert(Capacity > 1u, "RF packet ring needs at least two slots");

public:
    bool push(const T& item) {
        const uint32_t head = head_.load(std::memory_order_relaxed);
        const uint32_t tail = tail_.load(std::memory_order_acquire);
        if ((head - tail) >= Capacity) {
            drops_.fetch_add(1u, std::memory_order_relaxed);
            return false;
        }

        items_[head % Capacity] = item;
        head_.store(head + 1u, std::memory_order_release);
        return true;
    }

    bool pop(T& out) {
        const uint32_t tail = tail_.load(std::memory_order_relaxed);
        const uint32_t head = head_.load(std::memory_order_acquire);
        if (tail == head) return false;

        out = items_[tail % Capacity];
        tail_.store(tail + 1u, std::memory_order_release);
        return true;
    }

    uint32_t size() const {
        const uint32_t head = head_.load(std::memory_order_acquire);
        const uint32_t tail = tail_.load(std::memory_order_acquire);
        return head - tail;
    }

    uint32_t drops() const {
        return drops_.load(std::memory_order_relaxed);
    }

    /**
     * Reset only while the producer is disabled (before promiscuous start or
     * after promiscuous stop).
     */
    void reset() {
        tail_.store(0u, std::memory_order_relaxed);
        head_.store(0u, std::memory_order_relaxed);
        drops_.store(0u, std::memory_order_relaxed);
    }

private:
    T items_[Capacity] = {};
    std::atomic<uint32_t> head_{0u};
    std::atomic<uint32_t> tail_{0u};
    std::atomic<uint32_t> drops_{0u};
};

}  // namespace RfPacketRing
