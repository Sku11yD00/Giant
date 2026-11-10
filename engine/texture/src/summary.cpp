// A colour texture measured for the voxel world: its average colour and
// how much of it alpha cutoffs leave.
#include "giant/texture/texture.h"

#include <algorithm>
#include <cmath>

namespace giant::texture {

float ColorSummary::coverage_at(float cutoff) const {
    if (!(cutoff > 0.0f)) {
        return 1.0f; // also NaN
    }
    const float at = std::min(cutoff, 1.0f) * static_cast<float>(k_steps);
    const u32 below = std::min(static_cast<u32>(at), k_steps - 1);
    const float t = at - static_cast<float>(below);
    return coverage[below] + (coverage[below + 1] - coverage[below]) * t;
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
    const auto linear = [srgb](u8 b) {
        const float c = static_cast<float>(b) / 255.0f;
        if (!srgb) return c;
        return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
    };
    // BC1 as cooked carries no alpha (its 3-colour blocks' black is not a hole).
    std::array<u64, ColorSummary::k_steps + 1> at_or_over{};
    double sum[3] = {};
    double weight = 0.0;
    const usize count = usize{w} * h;
    for (usize i = 0; i < count; ++i) {
        const u8* t = &texels[i * 4];
        const u32 alpha = bc1 ? 255u : t[3];
        const double a = alpha / 255.0;
        for (u32 c = 0; c < 3; ++c) sum[c] += a * linear(t[c]);
        weight += a;
        // alpha / 255 >= k / 16, in integers.
        for (u32 k = 0; k <= ColorSummary::k_steps; ++k) {
            if (alpha * ColorSummary::k_steps >= k * 255u) ++at_or_over[k];
        }
    }
    if (weight > 0.0) {
        out.average = {static_cast<float>(sum[0] / weight), static_cast<float>(sum[1] / weight),
                       static_cast<float>(sum[2] / weight)};
    }
    for (u32 k = 0; k <= ColorSummary::k_steps; ++k) {
        out.coverage[k] = static_cast<float>(static_cast<double>(at_or_over[k]) / static_cast<double>(count));
    }
    return out;
}

} // namespace giant::texture
