// Multisampling's word a pixel (samples.slang writes it, shading and the
// terrain's texel pass read it): the surfaces an edge pixel's samples see.
// 0: one surface over the whole pixel. Else bits 0 to 3 the pixel's own
// surface's samples, then six bits for each other surface listed (at most
// three), from bit 4: the low three one of its samples, the high three how
// many it has (0 ends the list).
#pragma once

static const uint k_group_first = 4;
static const uint k_group_bits = 6;

uint group_own_samples(uint groups) {
    return groups & 0xFu;
}
// The `listed`-th other surface (0 to 2): one of its samples, and how many it has (0: no such surface).
uint group_sample(uint groups, uint listed) {
    return (groups >> (k_group_first + k_group_bits * listed)) & 7u;
}
uint group_samples(uint groups, uint listed) {
    return (groups >> (k_group_first + k_group_bits * listed + 3u)) & 7u;
}
