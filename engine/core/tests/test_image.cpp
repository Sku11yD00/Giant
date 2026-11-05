#include "giant/core/image.h"

#include "giant_test.h"

#include <vector>

using namespace giant;

namespace {
std::vector<u8> solid(u32 w, u32 h, u8 value) {
    return std::vector<u8>(static_cast<usize>(w) * h * 4, value);
}
} // namespace

GIANT_TEST("image: an image that fits is returned unchanged") {
    const auto pixels = solid(4, 2, 7);
    const ImageRgba8 out = downscale_rgba(pixels, 4, 2, 4);
    GIANT_CHECK(out.width == 4 && out.height == 2 && out.pixels == pixels);
}

GIANT_TEST("image: downscaling averages each box") {
    // 4x2, left half black and right half white, scaled to 2 wide: factor 2.
    std::vector<u8> pixels = solid(4, 2, 0);
    for (u32 y = 0; y < 2; ++y) {
        for (u32 x = 2; x < 4; ++x) {
            for (u32 c = 0; c < 4; ++c) {
                pixels[(y * 4 + x) * 4 + c] = 200;
            }
        }
    }
    const ImageRgba8 out = downscale_rgba(pixels, 4, 2, 2);
    GIANT_CHECK(out.width == 2 && out.height == 1);
    GIANT_CHECK(out.pixels.size() == 8 && out.pixels[0] == 0 && out.pixels[4] == 200);
}

GIANT_TEST("image: a short, wide image stays inside its pixels") {
    // Shorter than the scale factor (30): used to read past the end.
    const auto pixels = solid(1920, 10, 50);
    const ImageRgba8 out = downscale_rgba(pixels, 1920, 10, 64);
    GIANT_CHECK(out.width == 64 && out.height == 1);
    GIANT_CHECK(out.pixels.size() == 64 * 4 && out.pixels[0] == 50 && out.pixels.back() == 50);
}

GIANT_TEST("image: edge boxes cut short average only real pixels") {
    // 5x1 to at most 2 wide: factor 3, boxes [0,3) and [3,5).
    std::vector<u8> pixels = solid(5, 1, 0);
    for (u32 c = 0; c < 4; ++c) {
        pixels[3 * 4 + c] = 100;
        pixels[4 * 4 + c] = 100;
    }
    const ImageRgba8 out = downscale_rgba(pixels, 5, 1, 2);
    GIANT_CHECK(out.width == 2 && out.height == 1 && out.pixels[4] == 100);
}

GIANT_TEST("image: empty or mismatched input gives an empty image") {
    GIANT_CHECK(downscale_rgba({}, 0, 0, 64).pixels.empty());
    const auto pixels = solid(2, 2, 1);
    GIANT_CHECK(downscale_rgba(pixels, 3, 2, 64).pixels.empty());
}
