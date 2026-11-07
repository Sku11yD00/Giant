// Private: ranges of a few large GPU heaps handed out to small buffers.
//
// Every buffer made as its own allocation (a committed resource on D3D12)
// is new video memory: the OS finds it, zeroes it and maps it, and freeing
// it unmaps it again. That work runs on the GPU's timeline, and the
// renderer's queue waits behind it, so streaming that makes and drops
// thousands of small buffers (terrain tiles, meshes) stalls frames even
// though its own work is tiny. Small buffers are instead placed in a few
// heaps made once and kept: making one only takes a range here.
//
// Plain bookkeeping (no GPU), so it is tested on every platform: per heap,
// its free ranges in units, joined when neighbours free, best fit.
#pragma once

#include "giant/core/assert.h"
#include "giant/core/types.h"

#include <iterator>
#include <map>
#include <vector>

namespace giant::rhi {

class HeapRanges {
public:
    struct Range {
        u32 heap = ~0u; // ~0u: none
        u64 offset = 0; // units into it
        u64 units = 0;
        bool valid() const { return heap != ~0u; }
    };

    explicit HeapRanges(u64 units_per_heap) : m_units_per_heap(units_per_heap) {}

    u64 units_per_heap() const { return m_units_per_heap; }
    usize heaps() const { return m_free.size(); }
    u64 used() const { return m_used; } // units handed out, over every heap

    // A heap added (its index): all of it free.
    u32 add_heap() {
        m_free.emplace_back();
        m_free.back()[0] = m_units_per_heap;
        return static_cast<u32>(m_free.size() - 1);
    }

    // `units` from the heap whose best free range fits it most tightly; none
    // when no heap has room (add one, then ask again) or it is over a heap.
    Range allocate(u64 units) {
        Range best;
        u64 best_size = ~u64{0};
        if (units == 0 || units > m_units_per_heap) return best;
        for (u32 h = 0; h < m_free.size(); ++h) {
            for (const auto& [offset, size] : m_free[h]) {
                if (size >= units && size < best_size) {
                    best = {h, offset, units};
                    best_size = size;
                    if (size == units) break;
                }
            }
            if (best_size == units) break;
        }
        if (!best.valid()) return best;
        auto& free = m_free[best.heap];
        const auto it = free.find(best.offset);
        const u64 size = it->second;
        free.erase(it);
        if (size > units) free[best.offset + units] = size - units;
        m_used += units;
        return best;
    }

    // A range allocate gave, back (joined with free neighbours).
    void free(const Range& r) {
        if (!r.valid()) return;
        GIANT_ASSERT(r.heap < m_free.size() && r.offset + r.units <= m_units_per_heap, "not a range of ours");
        auto& free = m_free[r.heap];
        u64 offset = r.offset;
        u64 units = r.units;
        auto next = free.lower_bound(offset);
        GIANT_ASSERT(next == free.end() || next->first >= offset + units, "range freed twice");
        if (next != free.end() && next->first == offset + units) {
            units += next->second;
            next = free.erase(next);
        }
        if (next != free.begin()) {
            const auto before = std::prev(next);
            GIANT_ASSERT(before->first + before->second <= offset, "range freed twice");
            if (before->first + before->second == offset) {
                offset = before->first;
                units += before->second;
                free.erase(before);
            }
        }
        free[offset] = units;
        m_used -= r.units;
    }

private:
    u64 m_units_per_heap = 0;
    u64 m_used = 0;
    std::vector<std::map<u64, u64>> m_free; // per heap: offset -> units
};

} // namespace giant::rhi
