// Decoding images, block compression, and the .gtex file.
#include "giant/core/jobs.h"
#include "giant/texture/texture.h"

#include <bc7decomp.h>
#include <bc7enc.h>
#include <rgbcx.h>
#include <stb_image.h>

#include <algorithm>
#include <climits>
#include <cstring>
#include <mutex>

namespace giant::texture {

namespace {

void init_encoders() {
    static std::once_flag once;
    std::call_once(once, [] {
        rgbcx::init(rgbcx::bc1_approx_mode::cBC1Ideal);
        bc7enc_compress_block_init();
    });
}

rhi::Format format_for(const CookOptions& options) {
    switch (options.kind) {
    case Kind::color: return options.small ? rhi::Format::bc1_srgb : rhi::Format::bc7_srgb;
    case Kind::normal: return rhi::Format::bc5_unorm;
    case Kind::orm: return rhi::Format::bc7_unorm;
    case Kind::mask: return rhi::Format::bc4_unorm;
    }
    return rhi::Format::unknown;
}

// One 4x4 block of a level, edge texels repeated past its right and bottom.
void gather_block(const Image& level, u32 bx, u32 by, u8 out[64]) {
    for (u32 y = 0; y < 4; ++y) {
        const u32 sy = std::min(by * 4 + y, level.height - 1);
        for (u32 x = 0; x < 4; ++x) {
            const u32 sx = std::min(bx * 4 + x, level.width - 1);
            std::memcpy(out + (y * 4 + x) * 4, &level.rgba[(usize{sy} * level.width + sx) * 4], 4);
        }
    }
}

void encode_block(rhi::Format format, Kind kind, const u8 pixels[64], u8* dst) {
    switch (format) {
    case rhi::Format::bc1_srgb:
    case rhi::Format::bc1_unorm:
        rgbcx::encode_bc1(10, dst, pixels, true, false);
        break;
    case rhi::Format::bc4_unorm:
        rgbcx::encode_bc4_hq(dst, pixels, 4);
        break;
    case rhi::Format::bc5_unorm:
        rgbcx::encode_bc5_hq(dst, pixels, 0, 1, 4);
        break;
    case rhi::Format::bc7_srgb:
    case rhi::Format::bc7_unorm: {
        bc7enc_compress_block_params params;
        bc7enc_compress_block_params_init(&params);
        // Colour error is judged as the eye sees it; data channels each count alike.
        if (kind != Kind::color) bc7enc_compress_block_params_init_linear_weights(&params);
        bc7enc_compress_block(dst, pixels, &params);
        break;
    }
    default:
        break;
    }
}

u64 hash_contents(std::span<const u8> bytes) {
    // FNV-1a over 8-byte words, then the tail bytes.
    u64 h = 1469598103934665603ull;
    const usize words = bytes.size() / 8;
    for (usize i = 0; i < words; ++i) {
        u64 w = 0;
        std::memcpy(&w, bytes.data() + i * 8, 8);
        h = (h ^ w) * 1099511628211ull;
    }
    for (usize i = words * 8; i < bytes.size(); ++i) h = (h ^ bytes[i]) * 1099511628211ull;
    return h;
}

// Format codes in files, fixed whatever rhi's enum becomes.
struct FileFormat {
    u32 code;
    rhi::Format format;
};
constexpr FileFormat k_file_formats[] = {
    {1, rhi::Format::rgba8_unorm}, {2, rhi::Format::rgba8_srgb}, {3, rhi::Format::bc1_unorm},
    {4, rhi::Format::bc1_srgb},    {5, rhi::Format::bc4_unorm},  {6, rhi::Format::bc5_unorm},
    {7, rhi::Format::bc7_unorm},   {8, rhi::Format::bc7_srgb},
};

void put32(u8* p, u32 v) {
    for (u32 i = 0; i < 4; ++i) p[i] = static_cast<u8>(v >> (i * 8));
}
void put64(u8* p, u64 v) {
    for (u32 i = 0; i < 8; ++i) p[i] = static_cast<u8>(v >> (i * 8));
}
u32 get32(const u8* p) {
    u32 v = 0;
    for (u32 i = 0; i < 4; ++i) v |= static_cast<u32>(p[i]) << (i * 8);
    return v;
}
u64 get64(const u8* p) {
    u64 v = 0;
    for (u32 i = 0; i < 8; ++i) v |= static_cast<u64>(p[i]) << (i * 8);
    return v;
}

constexpr u8 k_magic[4] = {'G', 'T', 'E', 'X'};

} // namespace

bool decode_image(std::span<const u8> file, Image& out, std::string& error) {
    out = {};
    if (file.empty() || file.size() > static_cast<usize>(INT_MAX)) {
        error = "an image file is 1 byte to 2 GiB";
        return false;
    }
    const int length = static_cast<int>(file.size());
    int w = 0;
    int h = 0;
    int channels = 0;
    if (stbi_info_from_memory(file.data(), length, &w, &h, &channels) == 0) {
        error = std::string("not a PNG, JPEG, TGA or BMP image this reads: ") + stbi_failure_reason();
        return false;
    }
    if (w <= 0 || h <= 0 || w > 16384 || h > 16384) {
        error = "an image is 1 to 16384 texels a side";
        return false;
    }
    stbi_uc* pixels = stbi_load_from_memory(file.data(), length, &w, &h, &channels, 4);
    if (pixels == nullptr) {
        error = std::string("the image could not be decoded: ") + stbi_failure_reason();
        return false;
    }
    out.width = static_cast<u32>(w);
    out.height = static_cast<u32>(h);
    out.rgba.assign(pixels, pixels + usize{out.width} * out.height * 4);
    stbi_image_free(pixels);
    return true;
}

bool cook(const Image& image, const CookOptions& options, Cooked& out, std::string& error) {
    out = {};
    const std::vector<Image> levels = make_mips(image, options, error);
    if (levels.empty()) return false;
    init_encoders();

    out.kind = options.kind;
    out.desc.width = levels[0].width;
    out.desc.height = levels[0].height;
    out.desc.format = format_for(options);
    out.desc.usage = rhi::TextureUsage::sampled;
    out.desc.name = "cooked texture";
    out.desc.mips = static_cast<u32>(levels.size());
    out.desc.layers = 1;
    if (const char* problem = rhi::texture_desc_problem(out.desc)) {
        error = problem;
        out = {};
        return false;
    }
    out.contents.resize(rhi::texture_bytes(out.desc));

    // Every row of blocks of every level is one piece of work.
    struct Row {
        u32 mip;
        u32 by;
    };
    std::vector<Row> rows;
    for (u32 m = 0; m < out.desc.mips; ++m) {
        const u32 blocks_y = rhi::level_rows(out.desc.format, levels[m].height);
        for (u32 by = 0; by < blocks_y; ++by) rows.push_back({m, by});
    }
    const u32 block = rhi::block_bytes(out.desc.format);
    const auto encode_rows = [&](usize begin, usize end) {
        u8 pixels[64];
        for (usize r = begin; r < end; ++r) {
            const Image& level = levels[rows[r].mip];
            const u64 row_bytes = rhi::level_row_bytes(out.desc.format, level.width);
            u8* dst = out.contents.data() + level_offset(out.desc, 0, rows[r].mip) + row_bytes * rows[r].by;
            const u32 blocks_x = (level.width + 3) / 4;
            for (u32 bx = 0; bx < blocks_x; ++bx) {
                gather_block(level, bx, rows[r].by, pixels);
                encode_block(out.desc.format, options.kind, pixels, dst + usize{bx} * block);
            }
        }
    };
    if (options.jobs != nullptr) {
        options.jobs->parallel_for(rows.size(), 2, encode_rows);
    } else {
        encode_rows(0, rows.size());
    }
    return true;
}

std::vector<u8> decode_blocks(rhi::Format format, u32 width, u32 height, std::span<const u8> blocks) {
    if (!rhi::is_block_compressed(format) || width == 0 || height == 0 ||
        blocks.size() != rhi::level_bytes(format, width, height)) {
        return {};
    }
    init_encoders();
    std::vector<u8> out(usize{width} * height * 4);
    const u32 block = rhi::block_bytes(format);
    const u32 blocks_x = (width + 3) / 4;
    const u32 blocks_y = (height + 3) / 4;
    for (u32 by = 0; by < blocks_y; ++by) {
        for (u32 bx = 0; bx < blocks_x; ++bx) {
            const u8* src = blocks.data() + (usize{by} * blocks_x + bx) * block;
            u8 px[64] = {};
            switch (format) {
            case rhi::Format::bc1_unorm:
            case rhi::Format::bc1_srgb:
                rgbcx::unpack_bc1(src, px, true);
                break;
            case rhi::Format::bc4_unorm:
                rgbcx::unpack_bc4(src, px, 4);
                for (u32 i = 0; i < 16; ++i) {
                    px[i * 4 + 1] = px[i * 4 + 2] = px[i * 4];
                    px[i * 4 + 3] = 255;
                }
                break;
            case rhi::Format::bc5_unorm:
                rgbcx::unpack_bc5(src, px, 0, 1, 4);
                for (u32 i = 0; i < 16; ++i) {
                    px[i * 4 + 2] = 0;
                    px[i * 4 + 3] = 255;
                }
                break;
            case rhi::Format::bc7_unorm:
            case rhi::Format::bc7_srgb: {
                bc7decomp::color_rgba c[16];
                bc7decomp::unpack_bc7(src, c);
                for (u32 i = 0; i < 16; ++i) {
                    for (u32 k = 0; k < 4; ++k) px[i * 4 + k] = c[i][k];
                }
                break;
            }
            default:
                return {};
            }
            for (u32 y = 0; y < 4; ++y) {
                for (u32 x = 0; x < 4; ++x) {
                    const u32 tx = bx * 4 + x;
                    const u32 ty = by * 4 + y;
                    if (tx < width && ty < height) {
                        std::memcpy(&out[(usize{ty} * width + tx) * 4], px + (y * 4 + x) * 4, 4);
                    }
                }
            }
        }
    }
    return out;
}

u64 level_offset(const rhi::TextureDesc& desc, u32 layer, u32 mip) {
    u64 layer_bytes = 0;
    u64 within = 0;
    for (u32 m = 0; m < desc.mips; ++m) {
        const u64 bytes = rhi::level_bytes(desc.format, std::max(1u, desc.width >> m), std::max(1u, desc.height >> m));
        if (m < mip) within += bytes;
        layer_bytes += bytes;
    }
    return layer_bytes * layer + within;
}

std::vector<u8> write_file(const Cooked& texture) {
    u32 code = 0;
    for (const FileFormat& f : k_file_formats) {
        if (f.format == texture.desc.format) code = f.code;
    }
    std::vector<u8> file(k_file_header_bytes + texture.contents.size(), 0);
    u8* h = file.data();
    std::memcpy(h, k_magic, 4);
    put32(h + 4, k_file_version);
    put32(h + 8, static_cast<u32>(texture.kind));
    put32(h + 12, code);
    put32(h + 16, texture.desc.width);
    put32(h + 20, texture.desc.height);
    put32(h + 24, texture.desc.mips);
    put32(h + 28, texture.desc.layers);
    put32(h + 32, texture.desc.array ? 1u : 0u);
    put64(h + 40, texture.contents.size());
    put64(h + 48, hash_contents(texture.contents));
    if (!texture.contents.empty()) {
        std::memcpy(file.data() + k_file_header_bytes, texture.contents.data(), texture.contents.size());
    }
    return file;
}

bool read_file(std::span<const u8> file, Cooked& out, std::string& error) {
    out = {};
    if (file.size() < k_file_header_bytes || std::memcmp(file.data(), k_magic, 4) != 0) {
        error = "not a .gtex texture";
        return false;
    }
    const u8* h = file.data();
    if (get32(h + 4) != k_file_version) {
        error = "a .gtex texture of another version (" + std::to_string(get32(h + 4)) + ")";
        return false;
    }
    const u32 kind = get32(h + 8);
    if (kind > static_cast<u32>(Kind::mask)) {
        error = "a .gtex texture of an unknown kind";
        return false;
    }
    rhi::Format format = rhi::Format::unknown;
    for (const FileFormat& f : k_file_formats) {
        if (f.code == get32(h + 12)) format = f.format;
    }
    const u32 flags = get32(h + 32);
    if (format == rhi::Format::unknown || (flags & ~1u) != 0) {
        error = "a .gtex texture of an unknown format";
        return false;
    }
    Cooked t;
    t.kind = static_cast<Kind>(kind);
    t.desc.width = get32(h + 16);
    t.desc.height = get32(h + 20);
    t.desc.format = format;
    t.desc.usage = rhi::TextureUsage::sampled;
    t.desc.name = "gtex texture";
    t.desc.mips = get32(h + 24);
    t.desc.layers = get32(h + 28);
    t.desc.array = (flags & 1u) != 0;
    if (const char* problem = rhi::texture_desc_problem(t.desc)) {
        error = std::string("a .gtex texture rhi refuses: ") + problem;
        return false;
    }
    const u64 bytes = get64(h + 40);
    if (bytes == 0 || bytes != file.size() - k_file_header_bytes) {
        error = "a .gtex texture cut short or with extra bytes";
        return false;
    }
    if (const char* problem = rhi::texture_contents_problem(t.desc, bytes)) {
        error = std::string("a .gtex texture rhi refuses: ") + problem;
        return false;
    }
    const std::span<const u8> contents = file.subspan(k_file_header_bytes);
    if (hash_contents(contents) != get64(h + 48)) {
        error = "a .gtex texture whose contents fail their hash";
        return false;
    }
    t.contents.assign(contents.begin(), contents.end());
    out = std::move(t);
    return true;
}

} // namespace giant::texture
