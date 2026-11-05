// RGBA8 image helpers (screenshots).
#pragma once

#include "giant/core/types.h"

#include <span>
#include <vector>

namespace giant {

struct ImageRgba8 {
    u32 width = 0;
    u32 height = 0;
    std::vector<u8> pixels; // width * height * 4 bytes, rows top to bottom
};

// Box-filters an image down by a whole factor so it is at most `max_width`
// wide; returns it unchanged if it already fits. Every output pixel averages
// the source pixels it covers; boxes at the right and bottom edges may be
// cut short. Returns an empty image if the sizes do not match or are zero.
ImageRgba8 downscale_rgba(std::span<const u8> pixels, u32 width, u32 height, u32 max_width);

} // namespace giant
