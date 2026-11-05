#include "giant/core/png.h"

#include "giant_test.h"

#include <vector>

using namespace giant;

GIANT_TEST("png: writes a valid file header and refuses bad sizes") {
    const std::vector<u8> pixels(2 * 3 * 4, 200);
    const std::string png = encode_png_rgba(2, 3, pixels);
    GIANT_CHECK(png.size() > 8 + 25 + 12);
    GIANT_CHECK(png.compare(0, 8, std::string("\x89PNG\r\n\x1a\n", 8)) == 0);
    GIANT_CHECK(png.compare(12, 4, "IHDR") == 0);
    GIANT_CHECK(png.compare(png.size() - 8, 4, "IEND") == 0);
    // CRC of an empty IEND chunk is a well-known constant.
    GIANT_CHECK(static_cast<u8>(png[png.size() - 4]) == 0xAE &&
                static_cast<u8>(png[png.size() - 1]) == 0x82);
    GIANT_CHECK(encode_png_rgba(2, 3, std::vector<u8>(5)).empty());
    GIANT_CHECK(encode_png_rgba(0, 3, {}).empty());
}

GIANT_TEST("png: large images span several stored blocks") {
    const u32 w = 300;
    const u32 h = 100; // 120 KB of pixels: more than one 64 KB block
    std::vector<u8> pixels(static_cast<usize>(w) * h * 4);
    for (usize i = 0; i < pixels.size(); ++i) {
        pixels[i] = static_cast<u8>(i * 7);
    }
    const std::string png = encode_png_rgba(w, h, pixels);
    GIANT_CHECK(png.size() > pixels.size());
}
