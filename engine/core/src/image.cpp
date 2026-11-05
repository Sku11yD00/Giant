#include "giant/core/image.h"

#include <algorithm>

namespace giant {

ImageRgba8 downscale_rgba(std::span<const u8> pixels, u32 width, u32 height, u32 max_width) {
    ImageRgba8 out;
    if (width == 0 || height == 0 || max_width == 0 ||
        pixels.size() != static_cast<usize>(width) * height * 4) {
        return out;
    }
    if (width <= max_width) {
        out.width = width;
        out.height = height;
        out.pixels.assign(pixels.begin(), pixels.end());
        return out;
    }
    const u32 factor = (width + max_width - 1) / max_width;
    out.width = (width + factor - 1) / factor;
    out.height = (height + factor - 1) / factor;
    out.pixels.resize(static_cast<usize>(out.width) * out.height * 4);
    for (u32 y = 0; y < out.height; ++y) {
        const u32 y0 = y * factor;
        const u32 y1 = std::min(y0 + factor, height);
        for (u32 x = 0; x < out.width; ++x) {
            const u32 x0 = x * factor;
            const u32 x1 = std::min(x0 + factor, width);
            const u32 count = (y1 - y0) * (x1 - x0);
            for (u32 c = 0; c < 4; ++c) {
                u32 sum = 0;
                for (u32 sy = y0; sy < y1; ++sy) {
                    for (u32 sx = x0; sx < x1; ++sx) {
                        sum += pixels[(static_cast<usize>(sy) * width + sx) * 4 + c];
                    }
                }
                out.pixels[(static_cast<usize>(y) * out.width + x) * 4 + c] =
                    static_cast<u8>(sum / count);
            }
        }
    }
    return out;
}

} // namespace giant
