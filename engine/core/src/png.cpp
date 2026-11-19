#include "giant/core/png.h"

#include <algorithm>
#include <array>
#include <vector>

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

// --- Deflate (RFC 1951), enough of it: one block of fixed Huffman codes over an LZ77 pass. ---

// Bits go out lowest first; a Huffman code goes out highest bit first.
class BitWriter {
public:
    explicit BitWriter(std::string& out) : m_out(out) {}
    void bits(u32 value, u32 count) {
        m_hold |= static_cast<u64>(value) << m_held;
        m_held += count;
        while (m_held >= 8) {
            m_out += static_cast<char>(m_hold & 0xFF);
            m_hold >>= 8;
            m_held -= 8;
        }
    }
    void code(u32 value, u32 count) {
        u32 reversed = 0;
        for (u32 i = 0; i < count; ++i) {
            reversed |= ((value >> i) & 1u) << (count - 1 - i);
        }
        bits(reversed, count);
    }
    void finish() {
        if (m_held > 0) {
            m_out += static_cast<char>(m_hold & 0xFF);
            m_hold = 0;
            m_held = 0;
        }
    }

private:
    std::string& m_out;
    u64 m_hold = 0;
    u32 m_held = 0;
};

// A literal byte or a length code (0 to 287), in the fixed code.
void put_symbol(BitWriter& out, u32 symbol) {
    if (symbol < 144) {
        out.code(0x30 + symbol, 8);
    } else if (symbol < 256) {
        out.code(0x190 + (symbol - 144), 9);
    } else if (symbol < 280) {
        out.code(symbol - 256, 7);
    } else {
        out.code(0xC0 + (symbol - 280), 8);
    }
}

constexpr std::array<u16, 29> k_length_base = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                               31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr std::array<u8, 29> k_length_extra = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr std::array<u16, 30> k_distance_base = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                                 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr std::array<u8, 30> k_distance_extra = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

// "Copy `length` bytes from `distance` back" (3 to 258, 1 to 32768).
void put_match(BitWriter& out, u32 length, u32 distance) {
    usize l = k_length_base.size() - 1;
    while (k_length_base[l] > length) {
        --l;
    }
    put_symbol(out, 257 + static_cast<u32>(l));
    out.bits(length - k_length_base[l], k_length_extra[l]);
    usize d = k_distance_base.size() - 1;
    while (k_distance_base[d] > distance) {
        --d;
    }
    out.code(static_cast<u32>(d), 5);
    out.bits(distance - k_distance_base[d], k_distance_extra[d]);
}

// `raw` as a zlib stream (RFC 1950). Greedy matching over chains of the places three bytes were
// last seen, a few steps deep: fast rather than small, which suits a screenshot taken in a frame.
std::string deflate(const std::string& raw) {
    constexpr usize k_window = 32768;
    constexpr usize k_hash_size = usize{1} << 15;
    constexpr u32 k_none = 0xFFFFFFFFu;
    constexpr int k_chain = 12;
    constexpr usize k_min = 3, k_max = 258;
    std::string z;
    z.reserve(raw.size() / 4 + 64);
    z += static_cast<char>(0x78);
    z += static_cast<char>(0x01);
    BitWriter out(z);
    out.bits(1, 1); // the last block
    out.bits(1, 2); // of fixed codes
    std::vector<u32> head(k_hash_size, k_none);
    std::vector<u32> before(k_window, k_none); // the place before each of the last 32768, with the same three bytes
    const auto* data = reinterpret_cast<const u8*>(raw.data());
    const usize size = raw.size();
    const auto hash = [data](usize at) {
        const u32 three = static_cast<u32>(data[at]) | (static_cast<u32>(data[at + 1]) << 8) | (static_cast<u32>(data[at + 2]) << 16);
        return static_cast<usize>((three * 2654435761u) >> 17);
    };
    const auto remember = [&](usize at) {
        const usize h = hash(at);
        before[at % k_window] = head[h];
        head[h] = static_cast<u32>(at);
    };
    usize at = 0;
    while (at < size) {
        usize best = 0, best_distance = 0;
        if (at + k_min <= size) {
            const usize most = std::min(k_max, size - at);
            u32 candidate = head[hash(at)];
            for (int step = 0; step < k_chain && candidate != k_none; ++step) {
                const usize from = candidate;
                if (at - from > k_window) {
                    break;
                }
                usize length = 0;
                while (length < most && data[from + length] == data[at + length]) {
                    ++length;
                }
                if (length > best) {
                    best = length;
                    best_distance = at - from;
                    if (length == most) {
                        break;
                    }
                }
                const u32 next = before[from % k_window];
                if (next == k_none || next >= candidate) { // (written over since: no longer that chain)
                    break;
                }
                candidate = next;
            }
        }
        if (best >= k_min) {
            put_match(out, static_cast<u32>(best), static_cast<u32>(best_distance));
            for (usize i = 0; i < best; ++i) {
                if (at + i + k_min <= size) {
                    remember(at + i);
                }
            }
            at += best;
        } else {
            put_symbol(out, data[at]);
            if (at + k_min <= size) {
                remember(at);
            }
            ++at;
        }
    }
    put_symbol(out, 256); // the block's end
    out.finish();
    u32 a = 1;
    u32 b = 0;
    for (usize i = 0; i < size;) {
        // (5552 bytes at most before the sums could pass 32 bits.)
        const usize stop = std::min(size, i + 5552);
        for (; i < stop; ++i) {
            a += data[i];
            b += a;
        }
        a %= 65521;
        b %= 65521;
    }
    put_u32(z, (b << 16) | a);
    return z;
}

// How well a filtered row should pack: the sum of its bytes as distances from zero (the usual guess).
u32 row_cost(std::span<const u8> row) {
    u32 cost = 0;
    for (const u8 value : row) {
        cost += value < 128 ? value : 256u - value;
    }
    return cost;
}

} // namespace

std::string encode_png_rgba(u32 width, u32 height, std::span<const u8> rgba) {
    const usize row = static_cast<usize>(width) * 4;
    if (width == 0 || height == 0 || rgba.size() != row * height) {
        return {};
    }

    // The raw image: each row starts with its filter type, the one of none (0), sub (1: each byte
    // less the pixel to its left) and up (2: less the pixel above) that should pack best.
    std::string raw;
    raw.reserve((row + 1) * height);
    std::vector<u8> sub(row), up(row);
    for (u32 y = 0; y < height; ++y) {
        const std::span<const u8> line = rgba.subspan(static_cast<usize>(y) * row, row);
        for (usize i = 0; i < row; ++i) {
            sub[i] = static_cast<u8>(line[i] - (i >= 4 ? line[i - 4] : 0));
            up[i] = static_cast<u8>(line[i] - (y > 0 ? rgba[static_cast<usize>(y - 1) * row + i] : 0));
        }
        const u32 costs[3] = {row_cost(line), row_cost(sub), row_cost(up)};
        const usize filter = static_cast<usize>(std::min_element(costs, costs + 3) - costs);
        const std::span<const u8> chosen = filter == 0 ? line : filter == 1 ? std::span<const u8>(sub) : std::span<const u8>(up);
        raw += static_cast<char>(filter);
        raw.append(reinterpret_cast<const char*>(chosen.data()), row);
    }
    const std::string z = deflate(raw);

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
