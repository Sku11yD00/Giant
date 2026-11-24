// A set of indices, a bit each: instances still to be looked at, where a
// list would have to be sorted and merged as ranges come and go.
#pragma once

#include "giant/core/types.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <vector>

namespace giant::render {

class IndexSet {
public:
    static constexpr u32 k_none = ~u32{0};

    usize count() const { return m_count; }
    bool empty() const { return m_count == 0; }
    bool has(u32 i) const { return i / 64 < m_words.size() && (m_words[i / 64] >> (i % 64) & 1u) != 0; }

    void add(u32 i) {
        if (i / 64 >= m_words.size()) {
            m_words.resize(usize{i} / 64 + 1, 0);
        }
        const u64 bit = u64{1} << (i % 64);
        m_count += (m_words[i / 64] & bit) == 0 ? 1u : 0u;
        m_words[i / 64] |= bit;
    }
    // [first, end)
    void add_range(u32 first, u32 end) {
        if (first >= end) {
            return;
        }
        const usize last_word = (usize{end} - 1) / 64;
        if (last_word >= m_words.size()) {
            m_words.resize(last_word + 1, 0);
        }
        for (usize w = first / 64; w <= last_word; ++w) {
            const u64 lo = w * 64;
            const u64 from = std::max<u64>(first, lo) - lo; // 0..63
            const u64 to = std::min<u64>(end, lo + 64) - lo; // 1..64
            const u64 upto = to == 64 ? ~u64{0} : (u64{1} << to) - 1;
            const u64 mask = upto & ~((u64{1} << from) - 1);
            m_count += static_cast<usize>(std::popcount(mask & ~m_words[w]));
            m_words[w] |= mask;
        }
    }
    void remove(u32 i) {
        if (has(i)) {
            m_words[i / 64] &= ~(u64{1} << (i % 64));
            --m_count;
        }
    }
    void clear() {
        m_words.clear();
        m_count = 0;
    }
    // Those at `end` and past it are dropped.
    void keep_below(u32 end) {
        const usize words = (usize{end} + 63) / 64;
        for (usize w = words; w < m_words.size(); ++w) {
            m_count -= static_cast<usize>(std::popcount(m_words[w]));
        }
        if (words < m_words.size()) {
            m_words.erase(m_words.begin() + static_cast<std::ptrdiff_t>(words), m_words.end());
        }
        if (end % 64 != 0 && end / 64 < m_words.size()) {
            const u64 keep = (u64{1} << (end % 64)) - 1;
            m_count -= static_cast<usize>(std::popcount(m_words[end / 64] & ~keep));
            m_words[end / 64] &= keep;
        }
    }
    // The first in it at `from` or after; k_none: none.
    u32 next(u32 from) const {
        usize w = from / 64;
        if (w >= m_words.size()) {
            return k_none;
        }
        u64 word = m_words[w] & ~((u64{1} << (from % 64)) - 1);
        while (word == 0) {
            if (++w >= m_words.size()) {
                return k_none;
            }
            word = m_words[w];
        }
        return static_cast<u32>(w * 64 + static_cast<usize>(std::countr_zero(word)));
    }

private:
    std::vector<u64> m_words;
    usize m_count = 0;
};

} // namespace giant::render
