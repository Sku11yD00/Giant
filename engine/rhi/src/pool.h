// Private: the slot pool behind every handle type.
#pragma once

#include "giant/core/assert.h"
#include "giant/rhi/rhi.h"

#include <vector>

namespace giant::rhi {

// Stores T in slots addressed by Handle<Tag>. A destroyed slot's generation
// is bumped, so old handles to it stop resolving instead of reaching
// whatever was created there next. Slot 0 is never used: a zero handle is
// "none".
template <typename T, typename Tag>
class Pool {
public:
    using HandleType = Handle<Tag>;

    HandleType add(T value) {
        u32 index;
        if (!m_free.empty()) {
            index = m_free.back();
            m_free.pop_back();
        } else {
            index = static_cast<u32>(m_slots.size());
            m_slots.emplace_back();
        }
        Slot& slot = m_slots[index];
        slot.value = std::move(value);
        slot.alive = true;
        return {index, slot.generation};
    }

    // Null when the handle is none, stale or out of range.
    T* get(HandleType handle) {
        if (handle.index == 0 || handle.index >= m_slots.size()) {
            return nullptr;
        }
        Slot& slot = m_slots[handle.index];
        return slot.alive && slot.generation == handle.generation ? &slot.value : nullptr;
    }
    const T* get(HandleType handle) const { return const_cast<Pool*>(this)->get(handle); }

    // Moves the value out and frees the slot.
    T take(HandleType handle) {
        T* value = get(handle);
        GIANT_ASSERT(value != nullptr, "destroying a dead handle");
        Slot& slot = m_slots[handle.index];
        T out = std::move(slot.value);
        slot.value = T{};
        slot.alive = false;
        // Skip 0 on wrap-around: generation 0 means "none".
        slot.generation = slot.generation == 0xFFFF'FFFFu ? 1 : slot.generation + 1;
        m_free.push_back(handle.index);
        return out;
    }

    template <typename F>
    void for_each(F&& f) {
        for (usize i = 1; i < m_slots.size(); ++i) {
            if (m_slots[i].alive) {
                f(HandleType{static_cast<u32>(i), m_slots[i].generation}, m_slots[i].value);
            }
        }
    }

    usize live_count() const { return m_slots.size() - 1 - m_free.size(); }

private:
    struct Slot {
        T value{};
        u32 generation = 1;
        bool alive = false;
    };
    std::vector<Slot> m_slots = std::vector<Slot>(1); // slot 0 unused
    std::vector<u32> m_free;
};

} // namespace giant::rhi
