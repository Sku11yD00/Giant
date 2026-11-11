// A colour texture measured for the voxel world: its average colour and
// how much of it alpha cutoffs leave.
#include "giant/texture/texture.h"

#include <algorithm>
#include <cmath>

namespace giant::texture {

namespace {

// The step below a cutoff and how far past it (cutoff in (0, 1]).
void step_of(float cutoff, u32& below, float& t) {
    const float at = std::min(cutoff, 1.0f) * static_cast<float>(ColorSummary::k_steps);
    below = std::min(static_cast<u32>(at), ColorSummary::k_steps - 1);
    t = at - static_cast<float>(below);
}

} // namespace

float ColorSummary::coverage_at(float cutoff) const {
    if (!(cutoff > 0.0f)) {
        return 1.0f; // also NaN
    }
    u32 below = 0;
    float t = 0.0f;
    step_of(cutoff, below, t);
    return coverage[below] + (coverage[below + 1] - coverage[below]) * t;
}

Vec3 ColorSummary::average_at(float cutoff) const {
    if (!(cutoff > 0.0f)) {
        return average[0];
    }
    u32 below = 0;
    float t = 0.0f;
    step_of(cutoff, below, t);
    return average[below] + (average[below + 1] - average[below]) * t;
}

ColorSummary summarize_color(const Cooked& texture, u32 max_side) {
    ColorSummary out;
    const rhi::TextureDesc& desc = texture.desc;
    const rhi::Format format = desc.format;
    const bool bc7 = format == rhi::Format::bc7_srgb || format == rhi::Format::bc7_unorm;
    const bool bc1 = format == rhi::Format::bc1_srgb || format == rhi::Format::bc1_unorm;
    if (texture.kind != Kind::color || (!bc7 && !bc1) || desc.mips == 0 || desc.width == 0 || desc.height == 0) {
        return out;
    }
    // The finest level within max_side (the coarsest if none is).
    u32 mip = 0;
    while (mip + 1 < desc.mips && std::max(desc.width >> mip, desc.height >> mip) > std::max(max_side, 1u)) {
        ++mip;
    }
    const u32 w = std::max(desc.width >> mip, 1u);
    const u32 h = std::max(desc.height >> mip, 1u);
    const u64 offset = level_offset(desc, 0, mip);
    const u64 bytes = rhi::level_bytes(format, w, h);
    if (offset + bytes > texture.contents.size()) {
        return out;
    }
    const std::vector<u8> texels =
        decode_blocks(format, w, h, std::span<const u8>(texture.contents).subspan(offset, bytes));
    if (texels.empty()) {
        return out;
    }
    const bool srgb = format == rhi::Format::bc7_srgb || format == rhi::Format::bc1_srgb;
    std::array<float, 256> linear{};
    for (u32 i = 0; i < 256; ++i) {
        const float c = static_cast<float>(i) / 255.0f;
        linear[i] = !srgb ? c : c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
    }
    // Per alpha byte: how many texels and their colour sums, then summed
    // from the top down into the steps. (BC1 as cooked carries no alpha:
    // its 3-colour blocks' black is not a hole.)
    std::array<u64, 256> count_by_alpha{};
    std::array<std::array<double, 3>, 256> sum_by_alpha{};
    const usize count = usize{w} * h;
    for (usize i = 0; i < count; ++i) {
        const u8* t = &texels[i * 4];
        const u32 alpha = bc1 ? 255u : t[3];
        ++count_by_alpha[alpha];
        for (u32 c = 0; c < 3; ++c) sum_by_alpha[alpha][c] += linear[t[c]];
    }
    for (u32 k = 0; k <= ColorSummary::k_steps; ++k) {
        u64 n = 0;
        double sum[3] = {};
        for (u32 alpha = 0; alpha < 256; ++alpha) {
            // alpha / 255 >= k / 16, in integers.
            if (alpha * ColorSummary::k_steps < k * 255u) continue;
            n += count_by_alpha[alpha];
            for (u32 c = 0; c < 3; ++c) sum[c] += sum_by_alpha[alpha][c];
        }
        out.coverage[k] = static_cast<float>(static_cast<double>(n) / static_cast<double>(count));
        if (n > 0) {
            const double d = static_cast<double>(n);
            out.average[k] = {static_cast<float>(sum[0] / d), static_cast<float>(sum[1] / d),
                              static_cast<float>(sum[2] / d)};
        } else {
            out.average[k] = k > 0 ? out.average[k - 1] : Vec3{1.0f, 1.0f, 1.0f}; // none left: the step below's
        }
    }
    return out;
}

} // namespace giant::texture
