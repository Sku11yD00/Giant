// Height maps from other tools (World Machine, Gaea, image editors), as
// grey levels 0 to 1: RAW 16-bit (.r16 or .raw: little-endian, square)
// and PGM (.pgm: binary P5 or text P2, 8 or 16 bits).
#pragma once

#include "giant/core/types.h"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace giant::world {

struct HeightMap {
    u32 width = 0;
    u32 height = 0;
    std::vector<float> values; // 0 to 1, row by row from the top-left (width * height)

    // Bilinear, (u, v) from 0 to 1 across the image (clamped), v = 0 the first row.
    float at(double u, double v) const;
};

constexpr u32 k_max_height_map_side = 16384;

// Reads `bytes` as the format `extension` names (".r16", ".raw", ".pgm";
// any case). Returns an error and leaves `out` alone on failure.
std::optional<std::string> read_height_map(std::span<const u8> bytes, std::string_view extension, HeightMap& out);

} // namespace giant::world
