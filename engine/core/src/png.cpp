#include "giant/core/png.h"

#include <algorithm>
#include <array>

namespace giant {

namespace {

const std::array<u32, 256>& crc_table() {
    static const std::array<u32, 256> table = [] {
        std::array<u32, 256> t{};
        for (u32 n = 0; n < 256; ++n) {
            u32 c = n;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1) != 0 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            }
            t[n] = c;
        }
        return t;
    }();
    return table;
}

u32 crc32(const std::string& data, usize begin) {
    u32 c = 0xFFFFFFFFu;
    for (usize i = begin; i < data.size(); ++i) {
        c = crc_table()[(c ^ static_cast<u8>(data[i])) & 0xFF] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

void put_u32(std::string& out, u32 v) {
    out += static_cast<char>((v >> 24) & 0xFF);
    out += static_cast<char>((v >> 16) & 0xFF);
    out += static_cast<char>((v >> 8) & 0xFF);
    out += static_cast<char>(v & 0xFF);
}

void put_chunk(std::string& out, const char* type, const std::string& body) {
    put_u32(out, static_cast<u32>(body.size()));
    const usize start = out.size();
    out.append(type, 4);
    out += body;
    put_u32(out, crc32(out, start));
}

} // namespace

std::string encode_png_rgba(u32 width, u32 height, std::span<const u8> rgba) {
    const usize row = static_cast<usize>(width) * 4;
    if (width == 0 || height == 0 || rgba.size() != row * height) {
        return {};
    }

    // The raw image: each row starts with filter type 0 (none).
    std::string raw;
    raw.reserve((row + 1) * height);
    for (u32 y = 0; y < height; ++y) {
        raw += '\0';
        raw.append(reinterpret_cast<const char*>(rgba.data()) + static_cast<usize>(y) * row, row);
    }

    // zlib stream of stored (uncompressed) deflate blocks, with Adler-32.
    std::string z;
    z += static_cast<char>(0x78);
    z += static_cast<char>(0x01);
    constexpr usize k_block = 65535;
    for (usize at = 0; at < raw.size() || at == 0; at += k_block) {
        const usize n = std::min(k_block, raw.size() - at);
        const bool last = at + n >= raw.size();
        z += static_cast<char>(last ? 1 : 0);
        z += static_cast<char>(n & 0xFF);
        z += static_cast<char>((n >> 8) & 0xFF);
        z += static_cast<char>(~n & 0xFF);
        z += static_cast<char>((~n >> 8) & 0xFF);
        z.append(raw, at, n);
        if (last) {
            break;
        }
    }
    u32 a = 1;
    u32 b = 0;
    for (const char c : raw) {
        a = (a + static_cast<u8>(c)) % 65521;
        b = (b + a) % 65521;
    }
    put_u32(z, (b << 16) | a);

    std::string png("\x89PNG\r\n\x1a\n", 8);
    std::string header;
    put_u32(header, width);
    put_u32(header, height);
    header += static_cast<char>(8); // bit depth
    header += static_cast<char>(6); // colour type: RGBA
    header += '\0';                 // compression
    header += '\0';                 // filter
    header += '\0';                 // no interlace
    put_chunk(png, "IHDR", header);
    put_chunk(png, "IDAT", z);
    put_chunk(png, "IEND", {});
    return png;
}

} // namespace giant
