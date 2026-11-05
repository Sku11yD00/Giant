// SMAA's precomputed lookup textures (shaders/smaa.slang reads them).
#pragma once

#include "giant/rhi/rhi.h"

namespace giant::render {

struct SmaaTextures {
    rhi::TextureHandle area;   // 160 x 560, rg in rgba8_unorm
    rhi::TextureHandle search; // 64 x 16, r in rgba8_unorm
};

// False (nothing created) without GPU memory.
bool create_smaa_textures(rhi::Device& device, SmaaTextures& out);

} // namespace giant::render
