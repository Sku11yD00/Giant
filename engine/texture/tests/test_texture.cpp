// The texture cooker: decoding, mips per kind (linear light, alpha
// weighting, normals, roughness widened by normal detail), block encoding,
// and the .gtex file.
#include "giant/core/jobs.h"
#include "giant/core/png.h"
#include "giant/rhi/rhi.h"
#include "giant/texture/texture.h"

#include "giant_test.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>

using namespace giant;
using namespace giant::texture;

namespace {

Image filled(u32 w, u32 h, auto&& texel) {
    Image img;
    img.width = w;
    img.height = h;
    img.rgba.resize(usize{w} * h * 4);
    for (u32 y = 0; y < h; ++y) {
        for (u32 x = 0; x < w; ++x) {
            u8* t = &img.rgba[(usize{y} * w + x) * 4];
            texel(x, y, t);
        }
    }
    return img;
}

const u8* at(const Image& img, u32 x, u32 y) { return &img.rgba[(usize{y} * img.width + x) * 4]; }

// A gradient with some detail, as a colour texture might be.
Image colour_image(u32 w, u32 h) {
    return filled(w, h, [w, h](u32 x, u32 y, u8* t) {
        t[0] = static_cast<u8>(x * 255 / (w - 1));
        t[1] = static_cast<u8>(y * 255 / (h - 1));
        t[2] = static_cast<u8>(((x / 8 + y / 8) % 2) * 160 + 40);
        t[3] = 255;
    });
}

// Smooth shading of one hue with a little drift, as most photographed
// textures are: channels that move together.
Image smooth_image(u32 w, u32 h) {
    return filled(w, h, [](u32 x, u32 y, u8* t) {
        const double light = 0.5 + 0.4 * std::sin(x * 0.15 + y * 0.05);
        const double drift = 10.0 * std::cos(y * 0.12);
        t[0] = static_cast<u8>(230.0 * light + drift);
        t[1] = static_cast<u8>(180.0 * light + 20.0);
        t[2] = static_cast<u8>(120.0 * light + 20.0 - drift);
        t[3] = static_cast<u8>(200.0 + 50.0 * light);
    });
}

// Normals tilted 45 degrees left and right in alternate columns.
Image bumpy_normals(u32 w, u32 h) {
    return filled(w, h, [](u32 x, u32, u8* t) {
        const float s = 0.70710678f * (x % 2 == 0 ? 1.0f : -1.0f);
        t[0] = static_cast<u8>(std::lround((s * 0.5f + 0.5f) * 255.0f));
        t[1] = 128;
        t[2] = static_cast<u8>(std::lround((0.70710678f * 0.5f + 0.5f) * 255.0f));
        t[3] = 255;
    });
}

int largest_difference(std::span<const u8> a, std::span<const u8> b, u32 channels_of_4) {
    int worst = 0;
    for (usize i = 0; i < a.size(); ++i) {
        if (i % 4 >= channels_of_4) continue;
        worst = std::max(worst, std::abs(int{a[i]} - int{b[i]}));
    }
    return worst;
}

double mean_difference(std::span<const u8> a, std::span<const u8> b, u32 channels_of_4) {
    double sum = 0.0;
    usize count = 0;
    for (usize i = 0; i < a.size(); ++i) {
        if (i % 4 >= channels_of_4) continue;
        sum += std::abs(int{a[i]} - int{b[i]});
        ++count;
    }
    return count == 0 ? 0.0 : sum / static_cast<double>(count);
}

} // namespace

GIANT_TEST("texture: decodes a PNG and refuses what is not an image") {
    const Image src = colour_image(13, 7);
    const std::string png = encode_png_rgba(src.width, src.height, src.rgba);
    const std::span<const u8> bytes(reinterpret_cast<const u8*>(png.data()), png.size());
    Image out;
    std::string error;
    GIANT_CHECK(decode_image(bytes, out, error));
    GIANT_CHECK(out.width == 13 && out.height == 7);
    GIANT_CHECK(out.rgba == src.rgba);

    const u8 junk[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    GIANT_CHECK(!decode_image(junk, out, error));
    GIANT_CHECK(!error.empty());
    GIANT_CHECK(out.width == 0 && out.rgba.empty());
    GIANT_CHECK(!decode_image(bytes.first(bytes.size() / 2), out, error));
    GIANT_CHECK(!decode_image({}, out, error));
}

GIANT_TEST("texture: mips run to 1x1 from a top that is a multiple of 4 within max_size") {
    std::string error;
    const auto mips = make_mips(colour_image(64, 64), {}, error);
    GIANT_CHECK(mips.size() == 7);
    GIANT_CHECK(mips.back().width == 1 && mips.back().height == 1);

    const auto odd = make_mips(colour_image(30, 18), {}, error);
    GIANT_CHECK(!odd.empty() && odd[0].width == 32 && odd[0].height == 20);

    CookOptions small;
    small.max_size = 16;
    const auto fit = make_mips(colour_image(64, 40), small, error);
    GIANT_CHECK(!fit.empty() && fit[0].width == 16 && fit[0].height == 12);
    GIANT_CHECK(fit.size() == 5); // 16x12, 8x6, 4x3, 2x1, 1x1

    Image bad = colour_image(8, 8);
    bad.rgba.pop_back();
    GIANT_CHECK(make_mips(bad, {}, error).empty());
    GIANT_CHECK(!error.empty());
    GIANT_CHECK(make_mips(Image{}, {}, error).empty());
}

GIANT_TEST("texture: colour mips average light, not sRGB numbers") {
    // Black and white texels: half the light is sRGB 188, not 128.
    const Image checker = filled(8, 8, [](u32 x, u32 y, u8* t) {
        const u8 v = (x + y) % 2 == 0 ? 255 : 0;
        t[0] = t[1] = t[2] = v;
        t[3] = 255;
    });
    std::string error;
    const auto mips = make_mips(checker, {}, error);
    GIANT_CHECK(mips.size() == 4);
    GIANT_CHECK(std::abs(int{at(mips[1], 1, 1)[0]} - 188) <= 1);
    GIANT_CHECK(std::abs(int{at(mips[3], 0, 0)[1]} - 188) <= 1);
}

GIANT_TEST("texture: colour under transparent texels does not bleed into cut-out edges") {
    // Opaque red beside fully transparent green: the smaller mip stays red,
    // half covered.
    const Image leaf = filled(4, 4, [](u32 x, u32, u8* t) {
        const bool opaque = x % 2 == 0;
        t[0] = opaque ? 255 : 0;
        t[1] = opaque ? 0 : 255;
        t[2] = 0;
        t[3] = opaque ? 255 : 0;
    });
    std::string error;
    const auto mips = make_mips(leaf, {}, error);
    const u8* t = at(mips[1], 0, 0);
    GIANT_CHECK(t[0] == 255 && t[1] == 0);
    GIANT_CHECK(std::abs(int{t[3]} - 128) <= 1);

    // Nothing covered: the plain colour average is kept.
    const Image clear = filled(4, 4, [](u32, u32, u8* c) {
        c[0] = 10;
        c[1] = 200;
        c[2] = 30;
        c[3] = 0;
    });
    const auto clear_mips = make_mips(clear, {}, error);
    GIANT_CHECK(std::abs(int{at(clear_mips[2], 0, 0)[1]} - 200) <= 1);
    GIANT_CHECK(at(clear_mips[2], 0, 0)[3] == 0);
}

GIANT_TEST("texture: normal mips are renormalised averages, green flipped on request") {
    CookOptions options;
    options.kind = Kind::normal;
    std::string error;
    const auto mips = make_mips(bumpy_normals(8, 8), options, error);
    // Left and right tilts average to straight up, unit length.
    const u8* n = at(mips[1], 0, 0);
    GIANT_CHECK(std::abs(int{n[0]} - 128) <= 1 && std::abs(int{n[1]} - 128) <= 1 && n[2] >= 254);

    const Image up = filled(4, 4, [](u32, u32, u8* t) {
        t[0] = 128;
        t[1] = 218; // tilted toward +y (green up)
        t[2] = 218;
        t[3] = 255;
    });
    const auto same = make_mips(up, options, error);
    options.flip_green = true;
    const auto flipped = make_mips(up, options, error);
    GIANT_CHECK(at(same[0], 0, 0)[1] > 200);
    GIANT_CHECK(at(flipped[0], 0, 0)[1] < 56);
    // z is rebuilt from x and y, whatever blue held.
    GIANT_CHECK(std::abs(int{at(same[0], 0, 0)[2]} - int{at(flipped[0], 0, 0)[2]}) <= 1);
}

GIANT_TEST("texture: roughness widens where smaller mips average away normal detail") {
    // A glTF metallic-roughness image: roughness in G, metallic in B.
    const Image orm = filled(16, 16, [](u32, u32, u8* t) {
        t[0] = 200; // occlusion: not this texture's
        t[1] = 64;  // roughness 0.25
        t[2] = 180; // metallic
        t[3] = 255;
    });
    CookOptions options;
    options.kind = Kind::roughness_metal;
    std::string error;
    const auto plain = make_mips(orm, options, error);
    GIANT_CHECK(plain.size() == 5);
    for (const Image& level : plain) {
        GIANT_CHECK(std::abs(int{at(level, 0, 0)[0]} - 64) <= 1); // no normal map: unchanged
        GIANT_CHECK(std::abs(int{at(level, 0, 0)[1]} - 180) <= 1);
    }

    const Image bumps = bumpy_normals(16, 16);
    options.normal = &bumps;
    const auto widened = make_mips(orm, options, error);
    GIANT_CHECK(widened.size() == plain.size());
    // Full size keeps every bump: roughness as authored.
    GIANT_CHECK(std::abs(int{at(widened[0], 3, 3)[0]} - 64) <= 1);
    // Half size averages normals 45 degrees either way: |mean| = 0.704, so
    // alpha^2 = 0.25^4 + 2 (1 - 0.704) / 0.704 = 0.843, roughness 0.958.
    GIANT_CHECK(std::abs(int{at(widened[1], 1, 1)[0]} - 244) <= 2);
    GIANT_CHECK(at(widened[1], 1, 1)[1] == at(plain[1], 1, 1)[1]); // metallic untouched

    // A normal map at another size is matched by texel size: here the
    // texture's texels already span two bumps.
    const Image big_bumps = bumpy_normals(32, 32);
    options.normal = &big_bumps;
    const auto finer = make_mips(orm, options, error);
    GIANT_CHECK(std::abs(int{at(finer[0], 3, 3)[0]} - 244) <= 2);

    // Flat normals widen nothing.
    const Image flat = filled(16, 16, [](u32, u32, u8* t) {
        t[0] = t[1] = 128;
        t[2] = t[3] = 255;
    });
    options.normal = &flat;
    const auto none = make_mips(orm, options, error);
    GIANT_CHECK(std::abs(int{at(none.back(), 0, 0)[0]} - 64) <= 1);

    Image broken = flat;
    broken.width = 15;
    options.normal = &broken;
    GIANT_CHECK(make_mips(orm, options, error).empty());
}

GIANT_TEST("texture: cooks each kind into blocks that decode close to their mips") {
    struct Case {
        Kind kind;
        bool small;
        rhi::Format format;
        u32 channels;
        // Error bounds in 8-bit steps, per channel, on average and at worst:
        // loose enough for each format's limits, far below what blocks
        // written to the wrong place or channel give.
        double mean;
        int worst;
    };
    const Case cases[] = {
        {Kind::color, false, rhi::Format::bc7_srgb, 4, 3.5, 16},
        {Kind::color, true, rhi::Format::bc1_srgb, 3, 9.0, 32},
        {Kind::normal, false, rhi::Format::bc5_unorm, 2, 4.5, 16},
        {Kind::roughness_metal, false, rhi::Format::bc5_unorm, 2, 4.5, 16},
        {Kind::mask, false, rhi::Format::bc4_unorm, 1, 5.0, 16},
    };
    const Image image = smooth_image(24, 20);
    for (const Case& c : cases) {
        CookOptions options;
        options.kind = c.kind;
        options.small = c.small;
        std::string error;
        Cooked cooked;
        GIANT_CHECK(cook(image, options, cooked, error));
        GIANT_CHECK(cooked.kind == c.kind);
        GIANT_CHECK(cooked.desc.format == c.format);
        GIANT_CHECK(cooked.desc.width == 24 && cooked.desc.height == 20 && cooked.desc.mips == 5);
        GIANT_CHECK(rhi::texture_desc_problem(cooked.desc) == nullptr);
        GIANT_CHECK(rhi::texture_contents_problem(cooked.desc, cooked.contents.size()) == nullptr);

        const auto mips = make_mips(image, options, error);
        GIANT_CHECK(mips.size() == cooked.desc.mips);
        for (u32 m = 0; m < cooked.desc.mips && m < mips.size(); ++m) {
            const u64 offset = level_offset(cooked.desc, 0, m);
            const u64 bytes = rhi::level_bytes(c.format, mips[m].width, mips[m].height);
            const auto decoded = decode_blocks(c.format, mips[m].width, mips[m].height,
                                               std::span<const u8>(cooked.contents).subspan(offset, bytes));
            GIANT_CHECK(decoded.size() == mips[m].rgba.size());
            if (decoded.size() != mips[m].rgba.size()) continue;
            const int worst = largest_difference(decoded, mips[m].rgba, c.channels);
            const double mean = mean_difference(decoded, mips[m].rgba, c.channels);
            if (worst > c.worst || mean > c.mean) {
                std::fprintf(stderr, "    kind %d mip %u: off by %d, %.2f on average\n", static_cast<int>(c.kind), m,
                             worst, mean);
            }
            GIANT_CHECK(worst <= c.worst);
            GIANT_CHECK(mean <= c.mean);
        }
    }
    // A wrong size or format decodes to nothing.
    GIANT_CHECK(decode_blocks(rhi::Format::bc7_unorm, 4, 4, std::vector<u8>(15)).empty());
    GIANT_CHECK(decode_blocks(rhi::Format::rgba8_unorm, 1, 1, std::vector<u8>(4)).empty());
}

GIANT_TEST("texture: cooking on workers gives the same bytes, and the device takes them") {
    const Image image = colour_image(40, 32);
    std::string error;
    Cooked alone;
    GIANT_CHECK(cook(image, {}, alone, error));
    JobSystem jobs(3);
    CookOptions options;
    options.jobs = &jobs;
    Cooked spread;
    GIANT_CHECK(cook(image, options, spread, error));
    GIANT_CHECK(alone.contents == spread.contents);

    rhi::DeviceDesc desc;
    desc.backend = rhi::Backend::null;
    auto device = rhi::create_device(desc);
    GIANT_CHECK(device != nullptr);
    if (device) {
        const rhi::TextureHandle texture = device->create_texture(spread.desc, spread.contents);
        GIANT_CHECK(texture.valid());
        device->destroy(texture);
    }
}

GIANT_TEST("texture: level offsets walk layers and mips as rhi lays them out") {
    rhi::TextureDesc desc;
    desc.width = 16;
    desc.height = 8;
    desc.format = rhi::Format::bc7_unorm;
    desc.mips = 5;
    desc.layers = 3;
    desc.array = true;
    // 16x8: 8 blocks, 8x4: 2, 4x2: 1, 2x1: 1, 1x1: 1 = 13 blocks of 16 bytes.
    GIANT_CHECK(level_offset(desc, 0, 0) == 0);
    GIANT_CHECK(level_offset(desc, 0, 1) == 128);
    GIANT_CHECK(level_offset(desc, 0, 2) == 160);
    GIANT_CHECK(level_offset(desc, 1, 0) == 208);
    GIANT_CHECK(level_offset(desc, 2, 4) == 2 * 208 + 192);
    GIANT_CHECK(level_offset(desc, 3, 0) == rhi::texture_bytes(desc));
}

GIANT_TEST("texture: .gtex files round trip and bad ones are refused") {
    CookOptions options;
    options.kind = Kind::normal;
    std::string error;
    Cooked cooked;
    GIANT_CHECK(cook(bumpy_normals(16, 16), options, cooked, error));
    const std::vector<u8> file = write_file(cooked);
    GIANT_CHECK(file.size() == k_file_header_bytes + cooked.contents.size());

    Cooked back;
    GIANT_CHECK(read_file(file, back, error));
    GIANT_CHECK(back.kind == Kind::normal);
    GIANT_CHECK(back.desc.format == rhi::Format::bc5_unorm);
    GIANT_CHECK(back.desc.width == 16 && back.desc.height == 16 && back.desc.mips == 5);
    GIANT_CHECK(back.desc.layers == 1 && !back.desc.array);
    GIANT_CHECK(back.contents == cooked.contents);

    const auto refused = [&](std::vector<u8> bad) {
        Cooked out;
        std::string why;
        const bool ok = read_file(bad, out, why);
        return !ok && !why.empty() && out.contents.empty();
    };
    std::vector<u8> bad = file;
    bad[k_file_header_bytes + 5] ^= 0x40; // a flipped bit in the contents
    GIANT_CHECK(refused(bad));
    GIANT_CHECK(refused(std::vector<u8>(file.begin(), file.end() - 1))); // cut short
    GIANT_CHECK(refused(std::vector<u8>(file.begin(), file.begin() + 10)));
    bad = file;
    bad[0] = 'X'; // not a gtex
    GIANT_CHECK(refused(bad));
    bad = file;
    bad[4] = 9; // another version
    GIANT_CHECK(refused(bad));
    bad = file;
    bad[12] = 77; // unknown format
    GIANT_CHECK(refused(bad));
    bad = file;
    bad[16] = 13; // width 13: not a multiple of 4, rhi refuses
    GIANT_CHECK(refused(bad));
    bad = file;
    bad[24] = 4; // fewer mips than the contents hold
    GIANT_CHECK(refused(bad));
    bad = file;
    bad[8] = 9; // unknown kind
    GIANT_CHECK(refused(bad));
    bad = file;
    bad[8] = static_cast<u8>(Kind::color); // a normal map read as colour
    GIANT_CHECK(refused(bad));
    bad = file;
    bad[12] = 7; // BC7 for a normal map: never cooked that way
    GIANT_CHECK(refused(bad));
    bad = file;
    bad[8] = static_cast<u8>(Kind::roughness_metal); // also BC5: only the hash catches it
    GIANT_CHECK(refused(bad));
    bad = file;
    bad[37] = 1; // reserved
    GIANT_CHECK(refused(bad));
    bad = file;
    bad[60] = 1; // reserved
    GIANT_CHECK(refused(bad));
    bad = file;
    bad[32] = 2; // unknown flag
    GIANT_CHECK(refused(bad));

    // What a file could not take back is not written.
    Cooked odd = cooked;
    odd.contents.pop_back();
    GIANT_CHECK(write_file(odd).empty());
    odd = cooked;
    odd.desc.format = rhi::Format::r32_float;
    GIANT_CHECK(write_file(odd).empty());
    GIANT_CHECK(write_file(Cooked{}).empty());
    odd = cooked;
    odd.kind = Kind::color; // BC5 is never colour
    GIANT_CHECK(write_file(odd).empty());
}

GIANT_TEST("texture: roughness and metallic stay apart when they do not move together") {
    // Smooth roughness beside metallic in hard diagonal bands, as real
    // metal-and-paint textures are.
    const Image image = filled(32, 32, [](u32 x, u32 y, u8* t) {
        t[0] = static_cast<u8>((x * 37 + y * 11) % 256); // occlusion: ignored
        t[1] = static_cast<u8>(128.0 + 100.0 * std::sin(x * 0.2 + y * 0.07));
        t[2] = ((x + y) / 3) % 2 == 0 ? 255 : 0;
        t[3] = 255;
    });
    CookOptions options;
    options.kind = Kind::roughness_metal;
    std::string error;
    Cooked cooked;
    GIANT_CHECK(cook(image, options, cooked, error));
    GIANT_CHECK(cooked.desc.format == rhi::Format::bc5_unorm);
    const auto mips = make_mips(image, options, error);
    const auto decoded = decode_blocks(cooked.desc.format, 32, 32,
                                       std::span<const u8>(cooked.contents).first(rhi::level_bytes(cooked.desc.format, 32, 32)));
    GIANT_CHECK(decoded.size() == mips[0].rgba.size());
    if (decoded.size() == mips[0].rgba.size()) {
        GIANT_CHECK(largest_difference(decoded, mips[0].rgba, 2) <= 12);
        GIANT_CHECK(mean_difference(decoded, mips[0].rgba, 2) <= 3.0);
    }
}

GIANT_TEST("texture: a mask keeps the channel it is asked for") {
    const Image image = filled(8, 8, [](u32, u32, u8* t) {
        t[0] = 10;
        t[1] = 20;
        t[2] = 30;
        t[3] = 40;
    });
    CookOptions options;
    options.kind = Kind::mask;
    std::string error;
    for (u8 c = 0; c < 4; ++c) {
        options.mask_channel = c;
        const auto mips = make_mips(image, options, error);
        GIANT_CHECK(!mips.empty() && at(mips[0], 0, 0)[0] == 10 * (c + 1) && at(mips[0], 0, 0)[2] == 10 * (c + 1));
    }
    options.mask_channel = 4;
    GIANT_CHECK(make_mips(image, options, error).empty());
}

GIANT_TEST("texture: blocks decode as D3D lays them out") {
    // BC4: red endpoints, then 3-bit indices; index 0 is the first endpoint.
    const u8 bc4[8] = {200, 40, 0, 0, 0, 0, 0, 0};
    const auto one = decode_blocks(rhi::Format::bc4_unorm, 4, 4, bc4);
    GIANT_CHECK(one.size() == 64 && one[0] == 200 && one[60] == 200 && one[63] == 255);
    // Index 1 everywhere: the second endpoint.
    const u8 bc4_second[8] = {200, 40, 0x49, 0x92, 0x24, 0x49, 0x92, 0x24};
    const auto two = decode_blocks(rhi::Format::bc4_unorm, 4, 4, bc4_second);
    GIANT_CHECK(two.size() == 64 && two[0] == 40 && two[60] == 40);
    // BC5: the red block, then the green one.
    const u8 bc5[16] = {200, 40, 0, 0, 0, 0, 0, 0, 70, 30, 0, 0, 0, 0, 0, 0};
    const auto rg = decode_blocks(rhi::Format::bc5_unorm, 4, 4, bc5);
    GIANT_CHECK(rg.size() == 64 && rg[0] == 200 && rg[1] == 70 && rg[2] == 0);
    // BC1: two 565 colours, then 2-bit indices; index 0 is the first colour.
    const u8 bc1[8] = {0x00, 0xF8, 0x1F, 0x00, 0, 0, 0, 0}; // red 0xF800, blue 0x001F
    const auto rgb = decode_blocks(rhi::Format::bc1_unorm, 4, 4, bc1);
    GIANT_CHECK(rgb.size() == 64 && rgb[0] == 255 && rgb[1] == 0 && rgb[2] == 0 && rgb[3] == 255);
    // BC7 mode 6 (bit 6 set): a whole-block colour; checked as decodable.
    u8 bc7[16] = {};
    bc7[0] = 0x40;
    GIANT_CHECK(decode_blocks(rhi::Format::bc7_unorm, 4, 4, bc7).size() == 64);
}

GIANT_TEST("texture: cut-out colour mips keep the top level's alpha coverage") {
    // Scattered opaque texels, about 30% (leaves on a card): filtered
    // plainly, smaller levels' alpha sinks under the cutoff and they vanish.
    u32 seed = 12345;
    const Image leaves = filled(64, 64, [&seed](u32, u32, u8* t) {
        seed = seed * 1664525u + 1013904223u;
        t[0] = 40;
        t[1] = 160;
        t[2] = 30;
        t[3] = (seed >> 24) < 77u ? 255 : 0;
    });
    const auto covered = [](const Image& img) {
        usize kept = 0;
        for (usize i = 0; i < img.rgba.size(); i += 4) kept += img.rgba[i + 3] >= 128 ? 1u : 0u;
        return static_cast<double>(kept) / static_cast<double>(img.rgba.size() / 4);
    };
    std::string error;
    const auto kept = make_mips(leaves, {}, error);
    CookOptions plain_options;
    plain_options.coverage_cutoff = 0.0f;
    const auto plain = make_mips(leaves, plain_options, error);
    const double top = covered(kept[0]);
    GIANT_CHECK(top > 0.25 && top < 0.35 && covered(plain[0]) == top);
    for (usize level = 1; level <= 4; ++level) {
        GIANT_CHECK(std::abs(covered(kept[level]) - top) < 0.08);
    }
    GIANT_CHECK(covered(plain[3]) < 0.1); // thinned away
    // Colour is untouched by the alpha scale.
    GIANT_CHECK(at(kept[3], 0, 0)[1] == at(plain[3], 0, 0)[1]);
    // BC1 drops alpha: nothing to keep.
    CookOptions small_options;
    small_options.small = true;
    const auto small = make_mips(leaves, small_options, error);
    GIANT_CHECK(at(small[3], 0, 0)[3] == at(plain[3], 0, 0)[3]);
    // A 2x2 level takes the nearest count of texels (1 of 4), not more.
    GIANT_CHECK(covered(kept[5]) <= top);
    // Any cutoff keeps its coverage once stored in bytes.
    CookOptions third_options;
    third_options.coverage_cutoff = 0.33f;
    const auto third = make_mips(leaves, third_options, error);
    const auto covered_at = [](const Image& img, float cutoff) {
        usize kept_texels = 0;
        for (usize i = 0; i < img.rgba.size(); i += 4) kept_texels += img.rgba[i + 3] / 255.0f >= cutoff ? 1u : 0u;
        return static_cast<double>(kept_texels) / static_cast<double>(img.rgba.size() / 4);
    };
    for (usize level = 1; level <= 4; ++level) {
        GIANT_CHECK(std::abs(covered_at(third[level], 0.33f) - top) < 0.08);
    }
    // Opaque images are left as they are.
    const Image solid = filled(8, 8, [](u32, u32, u8* t) {
        t[0] = t[1] = t[2] = 100;
        t[3] = 255;
    });
    const auto solid_mips = make_mips(solid, {}, error);
    GIANT_CHECK(at(solid_mips[2], 0, 0)[3] == 255);
}
