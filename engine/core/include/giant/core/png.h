// Writes PNG images, for screenshots the AI and people can look at. Simple
// and dependency-free: the image data is stored without compression, so
// files are large; keep screenshots small (the editor scales them down).
#pragma once

#include "giant/core/types.h"

#include <span>
#include <string>

namespace giant {

// `rgba` holds width * height * 4 bytes, rows top to bottom. Returns the PNG
// file bytes, or an empty string if the sizes do not match.
std::string encode_png_rgba(u32 width, u32 height, std::span<const u8> rgba);

} // namespace giant
