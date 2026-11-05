#include "giant/rhi/rhi.h"

#include "backends.h"

#include "giant/core/log.h"

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
    }
    return 0;
}

bool is_depth(Format format) { return format == Format::depth32_float; }

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
