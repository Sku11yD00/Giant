#include "smaa.h"

#include <AreaTex.h>
#include <SearchTex.h>

#include <vector>

namespace giant::render {

namespace {

// The tables are one or two bytes a texel; the RHI's colour formats are
// four, so they are widened (a few hundred KB, once).
rhi::TextureHandle widen(rhi::Device& device, const unsigned char* bytes, u32 width, u32 height, u32 channels,
                         const char* name) {
    std::vector<u8> rgba(usize{width} * height * 4, 0);
    for (usize i = 0; i < usize{width} * height; ++i) {
        for (u32 c = 0; c < channels; ++c) {
            rgba[i * 4 + c] = bytes[i * channels + c];
        }
        rgba[i * 4 + 3] = 255;
    }
    rhi::TextureDesc desc;
    desc.width = width;
    desc.height = height;
    desc.format = rhi::Format::rgba8_unorm;
    desc.usage = rhi::TextureUsage::sampled;
    desc.name = name;
    return device.create_texture(desc, rgba);
}

} // namespace

bool create_smaa_textures(rhi::Device& device, SmaaTextures& out) {
    static_assert(sizeof(areaTexBytes) == AREATEX_SIZE && sizeof(searchTexBytes) == SEARCHTEX_SIZE);
    out.area = widen(device, areaTexBytes, AREATEX_WIDTH, AREATEX_HEIGHT, 2, "SMAA area texture");
    out.search = widen(device, searchTexBytes, SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, 1, "SMAA search texture");
    if (!out.area.valid() || !out.search.valid()) {
        device.destroy(out.area);
        device.destroy(out.search);
        out = {};
        return false;
    }
    return true;
}

} // namespace giant::render
