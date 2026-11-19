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

GIANT_TEST("png: the image is packed: what repeats takes little, and anything still fits") {
    const u32 w = 300;
    const u32 h = 100;
    // One colour: next to nothing.
    const std::vector<u8> flat(static_cast<usize>(w) * h * 4, 90);
    GIANT_CHECK(encode_png_rgba(w, h, flat).size() < flat.size() / 50);
    // A slope (each pixel a little more than the one before): the row filters make it repeat.
    std::vector<u8> slope(flat.size());
    for (usize i = 0; i < slope.size(); ++i) {
        slope[i] = static_cast<u8>((i / 4) % w + (i / 4) / w);
    }
    GIANT_CHECK(encode_png_rgba(w, h, slope).size() < slope.size() / 20);
    // Noise does not pack, and costs little more than its bytes (the fixed codes take 9 bits for some).
    std::vector<u8> noise(flat.size());
    u32 seed = 12345;
    for (u8& value : noise) {
        seed = seed * 1664525u + 1013904223u;
        value = static_cast<u8>(seed >> 24);
    }
    const usize packed = encode_png_rgba(w, h, noise).size();
    GIANT_CHECK(packed > noise.size() / 2 && packed < noise.size() + noise.size() / 7 + 1024);
    // The same pixels give the same file.
    GIANT_CHECK(encode_png_rgba(w, h, noise) == encode_png_rgba(w, h, noise));
}
