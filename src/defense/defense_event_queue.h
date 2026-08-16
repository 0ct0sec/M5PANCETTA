/** Fixed-capacity defense-event delivery policy. */
#pragma once

#include "defense_contracts.h"

namespace Defense {

template <size_t Capacity = MAX_EVENT_QUEUE,
          size_t CriticalReserve = EVENT_CRITICAL_RESERVE>
class EventQueue {
    static_assert(Capacity >= 2, "ring queue needs an empty sentinel slot");
    static_assert(CriticalReserve < Capacity - 1, "reserve must fit usable queue");

public:
    bool push(const DefenseEventData& event) {
        const uint8_t priority = eventPriority(event.event);
        const size_t maxUsable = Capacity - 1;
        if (priority <= 1 && size() >= maxUsable - CriticalReserve) return false;

        size_t next = increment(head_);
        if (next == tail_) {
            size_t victim = Capacity;
            for (size_t scan = tail_; scan != head_; scan = increment(scan)) {
                if (eventPriority(entries_[scan].event) < priority) {
                    victim = scan;
                    break;
                }
            }
            if (victim == Capacity) return false;
            removeAt(victim);
            next = increment(head_);
        }

        entries_[head_] = event;
        head_ = next;
        return true;
    }

    bool pop(DefenseEventData& out) {
        if (empty()) return false;
        out = entries_[tail_];
        tail_ = increment(tail_);
        return true;
    }

    bool empty() const { return head_ == tail_; }
    size_t size() const { return (head_ + Capacity - tail_) % Capacity; }

    void clear() {
        head_ = 0;
        tail_ = 0;
    }

private:
    static constexpr size_t increment(size_t value) { return (value + 1) % Capacity; }

    void removeAt(size_t index) {
        size_t current = index;
        while (current != head_) {
            const size_t next = increment(current);
            if (next == head_) break;
            entries_[current] = entries_[next];
            current = next;
        }
        head_ = (head_ + Capacity - 1) % Capacity;
    }

    DefenseEventData entries_[Capacity] = {};
    size_t head_ = 0;
    size_t tail_ = 0;
};

}  // namespace Defense
