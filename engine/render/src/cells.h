// Finding what changed by world cell (FrameInput::cells), for each part of
// the renderer that keeps its own copy of the instances (the voxel world,
// sun shadows): each compares with the cells it saw itself last, since it
// may skip frames.
#pragma once

#include "giant/render/renderer.h"

#include <span>
#include <vector>

namespace giant::render {

// Compares `now` with `seen` (what the caller saw last; updated to `now`).
// True when the cells are laid out as before: `changed` then lists the
// cells to look at (their version differs). False when every instance
// needs looking at: no cells, cells that do not cover the `count`
// instances end to end, or a different layout.
inline bool changed_cells(std::span<const CellRange> now, usize count, std::vector<CellRange>& seen,
                          std::vector<CellRange>& changed) {
    changed.clear();
    usize next = 0;
    for (const CellRange& c : now) {
        if (c.first != next) {
            seen.clear();
            return false;
        }
        next += c.count;
    }
    if (now.empty() || next != count) {
        seen.clear();
        return false;
    }
    bool same = seen.size() == now.size();
    for (usize i = 0; i < now.size() && same; ++i) {
        same = seen[i].first == now[i].first && seen[i].count == now[i].count;
    }
    if (same) {
        for (usize i = 0; i < now.size(); ++i) {
            if (seen[i].version != now[i].version) {
                changed.push_back(now[i]);
            }
        }
    }
    seen.assign(now.begin(), now.end());
    return same;
}

} // namespace giant::render
