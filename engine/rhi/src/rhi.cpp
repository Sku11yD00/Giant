#include "giant/rhi/rhi.h"

#include "backends.h"

#include "giant/core/log.h"

#include <algorithm>
#include <cstring>

namespace giant::rhi {

const char* to_string(Backend backend) {
    switch (backend) {
    case Backend::null: return "null";
    case Backend::d3d12: return "d3d12";
    }
    return "unknown";
}

Backend default_backend() {
#if GIANT_RHI_HAS_D3D12
    return Backend::d3d12;
#else
    return Backend::null;
#endif
}

std::unique_ptr<Device> create_device(const DeviceDesc& desc) {
    std::unique_ptr<Device> device;
    switch (desc.backend) {
    case Backend::null:
        device = create_null_device();
        break;
    case Backend::d3d12:
#if GIANT_RHI_HAS_D3D12
        device = create_d3d12_device(desc);
#else
        log_error("rhi", "the d3d12 backend is not built on this platform");
#endif
        break;
    }
    if (device == nullptr) {
        return nullptr;
    }

    const AdapterInfo& info = device->adapter();
    log_info("rhi", std::string("device: ") + to_string(device->backend()) + " on " + info.name +
                        (info.software ? " (software)" : "") + ", mesh shaders " +
                        (info.mesh_shaders ? "yes" : "no") + ", shader model 6.6 " +
                        (info.shader_model_6_6 ? "yes" : "no"));
    if (desc.backend != Backend::null && !info.meets_minimum_spec()) {
        log_warning("rhi", "this GPU is below the minimum spec (needs mesh shaders and shader "
                           "model 6.6); later milestones will not run on it");
    }
    return device;
}

u32 bytes_per_pixel(Format format) {
    switch (format) {
    case Format::unknown: return 0;
    case Format::rgba8_unorm:
    case Format::rgba8_srgb:
    case Format::r32_float:
    case Format::r32_uint:
    case Format::depth32_float: return 4;
    case Format::rgba16_float:
    case Format::rg32_uint: return 8;
    case Format::rgba32_float: return 16;
    case Format::bc1_unorm:
    case Format::bc1_srgb:
    case Format::bc4_unorm:
    case Format::bc5_unorm:
    case Format::bc7_unorm:
    case Format::bc7_srgb: return 0;
    }
    return 0;
}

bool is_depth(Format format) { return format == Format::depth32_float; }

u32 block_bytes(Format format) {
    switch (format) {
    case Format::bc1_unorm:
    case Format::bc1_srgb:
    case Format::bc4_unorm: return 8;
    case Format::bc5_unorm:
    case Format::bc7_unorm:
    case Format::bc7_srgb: return 16;
    default: return 0;
    }
}

bool is_block_compressed(Format format) { return block_bytes(format) != 0; }

u64 level_row_bytes(Format format, u32 width) {
    if (is_block_compressed(format)) {
        return (u64{width} + 3) / 4 * block_bytes(format);
    }
    return u64{width} * bytes_per_pixel(format);
}

u32 level_rows(Format format, u32 height) {
    return is_block_compressed(format) ? static_cast<u32>((u64{height} + 3) / 4) : height;
}

u64 level_bytes(Format format, u32 width, u32 height) {
    return level_row_bytes(format, width) * level_rows(format, height);
}

u32 full_mip_count(u32 width, u32 height) {
    u32 n = 1;
    for (u32 s = std::max(width, height); s > 1; s /= 2) {
        ++n;
    }
    return n;
}

u64 texture_bytes(const TextureDesc& desc) {
    u64 layer = 0;
    for (u32 m = 0; m < std::min(desc.mips, 32u); ++m) {
        layer += level_bytes(desc.format, std::max(desc.width >> m, 1u), std::max(desc.height >> m, 1u));
    }
    return layer * desc.layers;
}

u64 level_offset(const TextureDesc& desc, u32 mip) {
    u64 at = 0;
    for (u32 m = 0; m < std::min({mip, desc.mips, 32u}); ++m) {
        at += level_bytes(desc.format, std::max(desc.width >> m, 1u), std::max(desc.height >> m, 1u));
    }
    return at;
}

const char* texture_desc_problem(const TextureDesc& desc) {
    if (desc.width == 0 || desc.height == 0 || level_row_bytes(desc.format, 1) == 0) {
        return "a texture needs a size and a known format";
    }
    if (desc.width > 16384 || desc.height > 16384) {
        return "a texture is at most 16384 texels a side";
    }
    if (desc.usage == TextureUsage::none ||
        static_cast<u32>(desc.usage) >= (static_cast<u32>(TextureUsage::depth_target) << 1)) {
        return "a texture needs a known usage";
    }
    if (is_depth(desc.format) != has(desc.usage, TextureUsage::depth_target)) {
        return "depth formats are for depth targets and depth targets need depth formats";
    }
    if (is_depth(desc.format) &&
        (has(desc.usage, TextureUsage::render_target) || has(desc.usage, TextureUsage::shader_write))) {
        return "a depth texture cannot be a colour target or shader-written";
    }
    if (desc.mips == 0 || desc.mips > full_mip_count(desc.width, desc.height)) {
        return "a texture's mips run from 1 to its full chain";
    }
    if (desc.layers == 0 || desc.layers > 2048) {
        return "a texture has 1 to 2048 layers";
    }
    if (desc.layers > 1 && !desc.array) {
        return "a texture with more than one layer is an array";
    }
    if ((desc.mips > 1 || desc.layers > 1) && desc.usage != TextureUsage::sampled) {
        return "mips and layers are for sampled textures only";
    }
    if (is_block_compressed(desc.format)) {
        if (desc.usage != TextureUsage::sampled) {
            return "block-compressed textures are sampled only";
        }
        if (desc.width % 4 != 0 || desc.height % 4 != 0) {
            return "a block-compressed texture's size is a multiple of 4";
        }
    }
    if (texture_bytes(desc) > k_max_texture_bytes) {
        return "a texture is at most 4 GiB";
    }
    return nullptr;
}

const char* texture_contents_problem(const TextureDesc& desc, u64 contents_bytes) {
    if (contents_bytes == 0) {
        return nullptr;
    }
    return contents_bytes == texture_bytes(desc) ? nullptr : "a texture's contents do not fit its description";
}

namespace {

// A texture whose levels may be streamed: one sampled layer, nothing else.
bool streamable(const TextureDesc& desc) {
    return desc.usage == TextureUsage::sampled && desc.layers == 1 && !desc.array;
}

} // namespace

const char* write_texture_problem(const TextureDesc& desc, u32 mip, u32 first_row, u64 bytes) {
    if (!streamable(desc)) {
        return "only a sampled, single-layer texture is written by levels";
    }
    if (mip >= desc.mips) {
        return "write_texture: no such level";
    }
    const u32 w = std::max(desc.width >> mip, 1u);
    const u32 h = std::max(desc.height >> mip, 1u);
    const u64 row = level_row_bytes(desc.format, w);
    const u32 rows = level_rows(desc.format, h);
    if (bytes == 0 || row == 0 || bytes % row != 0) {
        return "write_texture: not whole rows of the level";
    }
    if (first_row >= rows || bytes / row > rows - first_row) {
        return "write_texture: rows past the level's end";
    }
    return nullptr;
}

const char* copy_levels_problem(const TextureDesc& dst, u32 dst_mip, const TextureDesc& src, u32 src_mip, u32 count) {
    if (!streamable(dst) || !streamable(src)) {
        return "only sampled, single-layer textures copy levels";
    }
    if (dst.format != src.format) {
        return "copy_texture_levels: the formats differ";
    }
    if (count == 0 || dst_mip >= dst.mips || src_mip >= src.mips || count > dst.mips - dst_mip ||
        count > src.mips - src_mip) {
        return "copy_texture_levels: no such levels";
    }
    for (u32 i = 0; i < count; ++i) {
        if (std::max(dst.width >> (dst_mip + i), 1u) != std::max(src.width >> (src_mip + i), 1u) ||
            std::max(dst.height >> (dst_mip + i), 1u) != std::max(src.height >> (src_mip + i), 1u)) {
            return "copy_texture_levels: the levels' sizes differ";
        }
    }
    return nullptr;
}

TransientMemory CommandList::upload(std::span<const u8> bytes) {
    TransientMemory memory = allocate(static_cast<u32>(bytes.size()));
    if (memory.data != nullptr && !bytes.empty()) {
        std::memcpy(memory.data, bytes.data(), bytes.size());
    }
    return memory;
}

Color clear_and_read_back(Device& device, const Color& color) {
    constexpr Color k_failed{-1.0f, -1.0f, -1.0f, -1.0f};
    TextureDesc desc;
    desc.width = 4;
    desc.height = 4;
    desc.format = Format::rgba8_unorm;
    desc.usage = TextureUsage::render_target;
    desc.name = "read-back check";
    const TextureHandle texture = device.create_texture(desc);
    if (!texture.valid()) {
        return k_failed;
    }
    CommandList* cmd = device.begin_frame(nullptr);
    if (cmd == nullptr) {
        device.destroy(texture);
        return k_failed;
    }
    RenderPassDesc pass;
    pass.colors[0] = {texture, LoadOp::clear, color};
    cmd->begin_render_pass(pass);
    cmd->end_render_pass();
    device.end_frame();
    const std::vector<u8> pixels = device.read_texture(texture);
    device.destroy(texture);
    if (pixels.size() < 4) {
        return k_failed;
    }
    const auto unorm = [&pixels](usize i) { return static_cast<float>(pixels[i]) / 255.0f; };
    return {unorm(0), unorm(1), unorm(2), unorm(3)};
}

std::vector<u8> render_ui_offscreen(Device& device, const UiFrame& ui, u32 width, u32 height,
                                    const Color& color) {
    if (width == 0 || height == 0) {
        return {};
    }
    TextureDesc desc;
    desc.width = width;
    desc.height = height;
    desc.format = Format::rgba8_unorm;
    desc.usage = TextureUsage::render_target;
    desc.name = "UI offscreen";
    const TextureHandle target = device.create_texture(desc);
    if (!target.valid()) {
        return {};
    }
    CommandList* cmd = device.begin_frame(nullptr);
    if (cmd == nullptr) {
        device.destroy(target);
        return {};
    }
    RenderPassDesc pass;
    pass.colors[0] = {target, LoadOp::clear, color};
    cmd->begin_render_pass(pass);
    cmd->end_render_pass();
    cmd->draw_ui(target, ui);
    device.end_frame();
    std::vector<u8> pixels = device.read_texture(target);
    device.destroy(target);
    return pixels;
}

} // namespace giant::rhi
