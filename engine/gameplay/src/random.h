// A small seeded random source for game rules: the same seed gives the
// same result on every platform (std distributions do not), so rolls can
// be tested and replayed.
#pragma once

#include "giant/core/types.h"

namespace giant::gameplay {

class Random {
public:
    explicit Random(u64 seed) : state_(seed) {}

    // splitmix64
    u64 next() {
        u64 z = (state_ += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    // 0 (included) to 1 (not included), 24 bits.
    float unit() { return static_cast<float>(next() >> 40) * (1.0f / 16777216.0f); }
    // -1 to 1.
    float signed_unit() { return unit() * 2.0f - 1.0f; }

private:
    u64 state_;
};

} // namespace giant::gameplay
