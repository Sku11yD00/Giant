#include "giant/world/height_map.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace giant::world {

float HeightMap::at(double u, double v) const {
    if (width == 0 || height == 0) return 0.0f;
    const double x = std::clamp(u, 0.0, 1.0) * (width - 1);
    const double y = std::clamp(v, 0.0, 1.0) * (height - 1);
    const u32 x0 = std::min(static_cast<u32>(x), width - 1);
    const u32 y0 = std::min(static_cast<u32>(y), height - 1);
    const u32 x1 = std::min(x0 + 1, width - 1);
    const u32 y1 = std::min(y0 + 1, height - 1);
    const double a = x - x0;
    const double b = y - y0;
    const auto p = [&](u32 i, u32 j) { return static_cast<double>(values[usize{j} * width + i]); };
    const double top = p(x0, y0) + (p(x1, y0) - p(x0, y0)) * a;
    const double bottom = p(x0, y1) + (p(x1, y1) - p(x0, y1)) * a;
    return static_cast<float>(top + (bottom - top) * b);
}

namespace {

// PGM header fields: whitespace and # comments between numbers.
struct Cursor {
    std::span<const u8> bytes;
    usize at = 0;
    void skip() {
        while (at < bytes.size()) {
            if (bytes[at] == '#') {
                while (at < bytes.size() && bytes[at] != '\n') ++at;
            } else if (std::isspace(bytes[at]) != 0) {
                ++at;
            } else {
                break;
            }
        }
    }
    std::optional<u32> number() {
        skip();
        u64 v = 0;
        usize digits = 0;
        while (at < bytes.size() && bytes[at] >= '0' && bytes[at] <= '9' && digits < 10) {
            v = v * 10 + (bytes[at] - '0');
            ++at;
            ++digits;
        }
        if (digits == 0 || v > 0xFFFFFFFFull) return std::nullopt;
        return static_cast<u32>(v);
    }
};

} // namespace

std::optional<std::string> read_height_map(std::span<const u8> bytes, std::string_view extension, HeightMap& out) {
    std::string ext(extension);
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    HeightMap map;
    if (ext == ".r16" || ext == ".raw") {
        if (bytes.empty() || bytes.size() % 2 != 0) return "a 16-bit RAW height map has two bytes a sample";
        const usize count = bytes.size() / 2;
        const auto side = static_cast<u32>(std::llround(std::sqrt(static_cast<double>(count))));
        if (usize{side} * side != count) {
            return "a RAW height map must be square (" + std::to_string(count) + " samples is not a square)";
        }
        if (side < 2 || side > k_max_height_map_side) {
            return "a height map is 2 to " + std::to_string(k_max_height_map_side) + " samples a side";
        }
        map.width = map.height = side;
        map.values.resize(count);
        for (usize k = 0; k < count; ++k) {
            const u32 v = u32{bytes[2 * k]} | (u32{bytes[2 * k + 1]} << 8);
            map.values[k] = static_cast<float>(v) / 65535.0f;
        }
    } else if (ext == ".pgm") {
        if (bytes.size() < 2 || bytes[0] != 'P' || (bytes[1] != '5' && bytes[1] != '2')) {
            return "a PGM height map starts with P5 (binary) or P2 (text)";
        }
        const bool text = bytes[1] == '2';
        Cursor c{bytes, 2};
        const auto w = c.number();
        const auto h = c.number();
        const auto max = c.number();
        if (!w || !h || !max) return "a PGM header is: width, height, largest value";
        if (*w < 2 || *h < 2 || *w > k_max_height_map_side || *h > k_max_height_map_side) {
            return "a height map is 2 to " + std::to_string(k_max_height_map_side) + " samples a side";
        }
        if (*max < 1 || *max > 65535) return "a PGM's largest value is 1 to 65535";
        map.width = *w;
        map.height = *h;
        const usize count = usize{*w} * *h;
        map.values.resize(count);
        if (text) {
            for (usize k = 0; k < count; ++k) {
                const auto v = c.number();
                if (!v || *v > *max) return "a PGM sample is a number from 0 to its largest value";
                map.values[k] = static_cast<float>(*v) / static_cast<float>(*max);
            }
        } else {
            // One whitespace byte, then the samples (big-endian when two bytes).
            if (c.at >= bytes.size() || std::isspace(bytes[c.at]) == 0) return "a PGM header ends in one space";
            ++c.at;
            const usize size = *max > 255 ? 2 : 1;
            if (bytes.size() - c.at < count * size) return "the PGM holds fewer samples than its header says";
            for (usize k = 0; k < count; ++k) {
                const usize p = c.at + k * size;
                const u32 v = size == 2 ? (u32{bytes[p]} << 8) | bytes[p + 1] : u32{bytes[p]};
                if (v > *max) return "a PGM sample is above its largest value";
                map.values[k] = static_cast<float>(v) / static_cast<float>(*max);
            }
        }
    } else {
        return "height maps are .r16 or .raw (16-bit, little-endian, square) or .pgm";
    }
    out = std::move(map);
    return std::nullopt;
}

} // namespace giant::world
