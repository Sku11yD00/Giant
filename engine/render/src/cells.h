// Finding what changed by world cell (FrameInput::cells), for each part of
// the renderer that keeps its own copy of the instances (the voxel world,
// sun shadows): each compares with the cells it saw itself last, since it
// may skip frames.
#pragma once

#include "giant/render/renderer.h"

#include <algorithm>
#include <span>
#include <vector>

namespace giant::render {

// Compares `now` with `seen` (what the caller saw last; updated to `now`).
// True when only some cells can differ: `changed` then lists the cells of
// `now` that are not exactly as seen (the same place, size and version),
// and instances outside them are as they were, at the same index. A cell
// seen at its prev_version is listed as just the part that changed. Cells
// may come, go, grow and move between frames: only those cells are
// listed. Instances past the seen ones are new, and always in a listed
// cell. False when every instance needs looking at: nothing seen yet, no
// cells, cells that do not cover the `count` instances end to end, or
// fewer instances than seen.
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
    usize seen_count = 0;
    for (const CellRange& c : seen) seen_count += c.count;
    const bool some = !seen.empty() && count >= seen_count;
    if (some) {
        // Both are in place order: walk them together.
        usize j = 0;
        for (const CellRange& c : now) {
            while (j < seen.size() && seen[j].first < c.first) ++j;
            bool same = false;
            bool from_prev = false; // seen as it was before only part of it changed
            for (usize k = j; k < seen.size() && seen[k].first == c.first && !same; ++k) {
                same = seen[k].count == c.count && seen[k].version == c.version; // empty cells share a place
                from_prev = from_prev || (c.prev_version != 0 && seen[k].count == c.count &&
                                          seen[k].version == c.prev_version);
            }
            if (!same) {
                if (from_prev && c.changed_first <= c.count) {
                    // Only the part that changed.
                    CellRange part = c;
                    part.first = c.first + c.changed_first;
                    part.count = std::min(c.changed_count, c.count - c.changed_first);
                    part.prev_version = 0;
                    changed.push_back(part);
                } else {
                    changed.push_back(c);
                }
            }
        }
    }
    seen.assign(now.begin(), now.end());
    return some;
}

} // namespace giant::render
