#include "giant/rhi/rhi.h"

#include "giant/core/assert.h"
#include "giant_test.h"
#include "heap_ranges.h"
#include "rhi_test_shaders.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include <vector>

using namespace giant;
using namespace giant::rhi;

namespace {

std::unique_ptr<Device> null_device() {
    DeviceDesc desc;
    desc.backend = Backend::null;
    return create_device(desc);
}

void check_color_near(const Color& got, const Color& want) {
    // 8-bit render targets: allow one step of rounding.
    const double tolerance = 1.0 / 255.0 + 1e-6;
    GIANT_CHECK_NEAR(got.r, want.r, tolerance);
    GIANT_CHECK_NEAR(got.g, want.g, tolerance);
    GIANT_CHECK_NEAR(got.b, want.b, tolerance);
    GIANT_CHECK_NEAR(got.a, want.a, tolerance);
}

TextureHandle white_ui_texture(Device& device) {
    TextureDesc desc;
    desc.width = 1;
    desc.height = 1;
    desc.format = Format::rgba8_unorm;
    desc.usage = TextureUsage::sampled;
    desc.name = "white";
    const std::vector<u8> white(4, 255);
    return device.create_texture(desc, white);
}

// A UI frame with one rectangle, for the UI drawing tests.
struct UiQuad {
    std::vector<UiVertex> vertices;
    std::vector<u32> indices{0, 1, 2, 0, 2, 3};
    std::vector<UiDrawCommand> commands;
    std::vector<UiDrawList> lists;

    UiQuad(float x0, float y0, float x1, float y1, u32 color, TextureId texture,
           float clip_x1 = 1e9f) {
        vertices = {{x0, y0, 0.0f, 0.0f, color},
                    {x1, y0, 1.0f, 0.0f, color},
                    {x1, y1, 1.0f, 1.0f, color},
                    {x0, y1, 0.0f, 1.0f, color}};
        UiDrawCommand cmd;
        cmd.index_count = 6;
        cmd.clip_max_x = clip_x1;
        cmd.clip_max_y = 1e9f;
        cmd.texture = texture;
        commands = {cmd};
        lists = {{vertices, indices, commands}};
    }
    UiFrame frame() const { return {lists}; }
};

constexpr u32 rgba(u32 r, u32 g, u32 b, u32 a) { return r | (g << 8) | (b << 16) | (a << 24); }

// One RGBA8 pixel of an image `width` pixels wide.
std::array<int, 4> pixel(const std::vector<u8>& image, u32 width, u32 x, u32 y) {
    const usize i = (static_cast<usize>(y) * width + x) * 4;
    return {image[i], image[i + 1], image[i + 2], image[i + 3]};
}

bool near(const std::array<int, 4>& got, const std::array<int, 4>& want, int tolerance = 2) {
    for (usize i = 0; i < 4; ++i) {
        if (std::abs(got[i] - want[i]) > tolerance) {
            return false;
        }
    }
    return true;
}

std::vector<u32> as_u32(const std::vector<u8>& bytes) {
    std::vector<u32> out(bytes.size() / 4);
    std::memcpy(out.data(), bytes.data(), out.size() * 4);
    return out;
}

int g_asserts = 0;
void counting_handler(const char*, const char*, const char*, int) { ++g_asserts; }

// Counts asserts while alive, so misuse tests can check the null backend
// catches mistakes (it must stay safe to continue after each one).
struct CountAsserts {
    CountAsserts() {
        g_asserts = 0;
        set_assert_handler(&counting_handler);
    }
    ~CountAsserts() { set_assert_handler(nullptr); }
    int count() const { return g_asserts; }
};

} // namespace

// --- Null backend -------------------------------------------------------------

GIANT_TEST("rhi: the null device runs frames on a swap chain") {
    auto device = null_device();
    GIANT_CHECK(device != nullptr && device->backend() == Backend::null);
    if (device == nullptr) {
        return;
    }
    SwapChainDesc sc_desc;
    sc_desc.width = 640;
    sc_desc.height = 360;
    auto swap_chain = device->create_swap_chain(sc_desc);
    GIANT_CHECK(swap_chain != nullptr);
    if (swap_chain == nullptr) {
        return;
    }
    for (int i = 0; i < 3; ++i) {
        CommandList* cmd = device->begin_frame(swap_chain.get());
        GIANT_CHECK(cmd != nullptr);
        if (cmd == nullptr) {
            return;
        }
        RenderPassDesc pass;
        pass.colors[0] = {swap_chain->current_texture(), LoadOp::clear, {0.2f, 0.4f, 0.6f, 1.0f}};
        cmd->begin_render_pass(pass);
        cmd->end_render_pass();
        device->end_frame();
    }
    GIANT_CHECK(device->frames_completed() == 3);
    GIANT_CHECK(!device->device_lost());

    const TextureDesc* back = device->describe(swap_chain->current_texture());
    GIANT_CHECK(back != nullptr && back->format == Format::rgba8_srgb && back->width == 640);

    swap_chain->resize(0, 0); // minimised: ignored
    GIANT_CHECK(swap_chain->width() == 640);
    swap_chain->resize(800, 600);
    GIANT_CHECK(swap_chain->width() == 800 && swap_chain->height() == 600);
    back = device->describe(swap_chain->current_texture());
    GIANT_CHECK(back != nullptr && back->width == 800 && back->height == 600);
}

GIANT_TEST("rhi: the null device reads back what it cleared") {
    auto device = null_device();
    GIANT_CHECK(device != nullptr);
    if (device != nullptr) {
        check_color_near(clear_and_read_back(*device, {1.0f, 0.5f, 0.0f, 1.0f}),
                         {1.0f, 0.5f, 0.0f, 1.0f});
    }
}

GIANT_TEST("rhi: clears store sRGB-encoded colour and reverse-Z depth") {
    auto device = null_device();
    if (device == nullptr) {
        GIANT_CHECK(false);
        return;
    }
    TextureDesc color_desc;
    color_desc.width = 2;
    color_desc.height = 2;
    color_desc.format = Format::rgba8_srgb;
    color_desc.usage = TextureUsage::render_target;
    const TextureHandle color = device->create_texture(color_desc);
    TextureDesc depth_desc = color_desc;
    depth_desc.format = Format::depth32_float;
    depth_desc.usage = TextureUsage::depth_target;
    const TextureHandle depth = device->create_texture(depth_desc);
    GIANT_CHECK(color.valid() && depth.valid());

    CommandList* cmd = device->begin_frame(nullptr);
    RenderPassDesc pass;
    pass.colors[0] = {color, LoadOp::clear, {0.5f, 0.0f, 1.0f, 1.0f}};
    pass.depth = {depth, LoadOp::clear, 0.0f};
    cmd->begin_render_pass(pass);
    cmd->end_render_pass();
    device->end_frame();

    // Linear 0.5 is sRGB byte 188.
    const auto pixels = device->read_texture(color);
    GIANT_CHECK(pixels.size() == 16);
    if (pixels.size() == 16) {
        GIANT_CHECK(near(pixel(pixels, 2, 1, 1), {188, 0, 255, 255}, 1));
    }
    const auto depths = device->read_texture(depth);
    GIANT_CHECK(depths.size() == 16);
    if (depths.size() == 16) {
        float d = 1.0f;
        std::memcpy(&d, depths.data() + 12, 4);
        GIANT_CHECK(d == 0.0f);
    }
    device->destroy(color);
    device->destroy(depth);
}

GIANT_TEST("rhi: stale handles stop resolving after destroy") {
    auto device = null_device();
    if (device == nullptr) {
        GIANT_CHECK(false);
        return;
    }
    BufferDesc desc;
    desc.size = 64;
    const BufferHandle first = device->create_buffer(desc);
    GIANT_CHECK(first.valid() && device->describe(first) != nullptr);
    GIANT_CHECK(device->describe(first)->size == 64);
    device->destroy(first);
    GIANT_CHECK(device->describe(first) == nullptr);
    GIANT_CHECK(device->read_descriptor(first) == k_no_descriptor);

    // The slot is reused under a new generation; the old handle stays dead.
    const BufferHandle second = device->create_buffer(desc);
    GIANT_CHECK(second.index == first.index && second.generation != first.generation);
    GIANT_CHECK(device->describe(first) == nullptr && device->describe(second) != nullptr);

    // "None" is never a live resource, and destroying it is harmless.
    GIANT_CHECK(!BufferHandle{}.valid() && device->describe(BufferHandle{}) == nullptr);
    device->destroy(BufferHandle{});
    device->destroy(second);

    // UI texture ids carry the generation too.
    const TextureHandle texture = white_ui_texture(*device);
    GIANT_CHECK(texture_from_ui_id(ui_texture_id(texture)) == texture);
    GIANT_CHECK(ui_texture_id(texture) != 0);
    device->destroy(texture);
}

GIANT_TEST("rhi: descriptors exist only for the views a resource has") {
    auto device = null_device();
    if (device == nullptr) {
        GIANT_CHECK(false);
        return;
    }
    BufferDesc read_only;
    read_only.size = 16;
    read_only.usage = BufferUsage::shader_read;
    BufferDesc read_write = read_only;
    read_write.usage = BufferUsage::shader_read | BufferUsage::shader_write;
    BufferDesc index_only = read_only;
    index_only.usage = BufferUsage::index;

    const BufferHandle a = device->create_buffer(read_only);
    const BufferHandle b = device->create_buffer(read_write);
    const BufferHandle c = device->create_buffer(index_only);
    GIANT_CHECK(device->read_descriptor(a) != k_no_descriptor);
    GIANT_CHECK(device->write_descriptor(a) == k_no_descriptor);
    GIANT_CHECK(device->read_descriptor(b) != k_no_descriptor);
    GIANT_CHECK(device->write_descriptor(b) != k_no_descriptor);
    GIANT_CHECK(device->read_descriptor(b) != device->write_descriptor(b));
    GIANT_CHECK(device->read_descriptor(c) == k_no_descriptor);
    GIANT_CHECK(device->write_descriptor(c) == k_no_descriptor);

    TextureDesc target;
    target.width = 4;
    target.height = 4;
    target.format = Format::rgba16_float;
    target.usage = TextureUsage::render_target | TextureUsage::sampled;
    const TextureHandle t = device->create_texture(target);
    GIANT_CHECK(device->read_descriptor(t) != k_no_descriptor);
    GIANT_CHECK(device->write_descriptor(t) == k_no_descriptor);

    // Freed descriptors are reused, so the heap does not leak.
    const u32 freed = device->read_descriptor(a);
    device->destroy(a);
    const BufferHandle d = device->create_buffer(read_only);
    GIANT_CHECK(device->read_descriptor(d) == freed);

    device->destroy(b);
    device->destroy(c);
    device->destroy(d);
    device->destroy(t);
}

GIANT_TEST("rhi: buffers start with their initial data and copy on the GPU timeline") {
    auto device = null_device();
    if (device == nullptr) {
        GIANT_CHECK(false);
        return;
    }
    const std::vector<u32> values{1, 2, 3, 4};
    BufferDesc gpu_desc;
    gpu_desc.size = 32; // larger than the data: the rest is zero
    const BufferHandle gpu = device->create_buffer(
        gpu_desc, {reinterpret_cast<const u8*>(values.data()), values.size() * 4});
    GIANT_CHECK(as_u32(device->read_buffer(gpu)) == (std::vector<u32>{1, 2, 3, 4, 0, 0, 0, 0}));

    BufferDesc upload_desc;
    upload_desc.size = 16;
    upload_desc.memory = Memory::upload;
    const BufferHandle upload = device->create_buffer(upload_desc);
    u8* mapped = device->mapped(upload);
    GIANT_CHECK(mapped != nullptr);
    const std::array<u32, 4> more{9, 8, 7, 6};
    std::memcpy(mapped, more.data(), 16);

    BufferDesc readback_desc;
    readback_desc.size = 32;
    readback_desc.memory = Memory::readback;
    const BufferHandle readback = device->create_buffer(readback_desc);

    CommandList* cmd = device->begin_frame(nullptr);
    cmd->barrier(gpu, ResourceState::copy_dest);
    cmd->copy_buffer(gpu, 16, upload, 0, 16); // upload buffers need no barrier
    cmd->barrier(gpu, ResourceState::copy_source);
    cmd->copy_buffer(readback, 0, gpu, 0, 32);
    device->end_frame();
    GIANT_CHECK(as_u32(device->read_buffer(readback)) ==
                (std::vector<u32>{1, 2, 3, 4, 9, 8, 7, 6}));
    GIANT_CHECK(device->last_frame_stats().barriers == 2);

    device->destroy(gpu);
    device->destroy(upload);
    device->destroy(readback);
}

GIANT_TEST("rhi: transient memory is aligned, grows and is counted") {
    auto device = null_device();
    if (device == nullptr) {
        GIANT_CHECK(false);
        return;
    }
    CommandList* cmd = device->begin_frame(nullptr);
    const TransientMemory a = cmd->allocate(4);
    const TransientMemory b = cmd->allocate(20);
    GIANT_CHECK(a.data != nullptr && b.data != nullptr);
    GIANT_CHECK(a.descriptor != k_no_descriptor);
    GIANT_CHECK(a.offset % k_transient_alignment == 0 && b.offset % k_transient_alignment == 0);
    GIANT_CHECK(b.buffer == a.buffer && b.offset == a.offset + k_transient_alignment);
    GIANT_CHECK(a.size == 4 && b.size == 20);

    // Bigger than what is left: a new block, while the old pointers stay
    // valid until the frame ends.
    std::memset(a.data, 0xAB, 4);
    const TransientMemory big = cmd->allocate(4u << 20);
    GIANT_CHECK(big.data != nullptr && big.size == 4u << 20);
    std::memset(big.data, 0, big.size);
    GIANT_CHECK(a.data[0] == 0xAB);

    const std::array<u32, 3> items{5, 6, 7};
    const TransientMemory uploaded = cmd->upload_array(std::span<const u32>(items));
    u32 copy[3] = {};
    std::memcpy(copy, uploaded.data, 12);
    GIANT_CHECK(copy[0] == 5 && copy[2] == 7);
    device->end_frame();
    // The same alignment as D3D12, so budgets measured on either agree.
    GIANT_CHECK(device->last_frame_stats().transient_bytes == 3 * k_transient_alignment + (4u << 20));
}

GIANT_TEST("rhi: the null device checks and counts draws, dispatches and UI") {
    auto device = null_device();
    if (device == nullptr) {
        GIANT_CHECK(false);
        return;
    }
    const TextureHandle white = white_ui_texture(*device);
    GIANT_CHECK(white.valid());

    UiQuad quad(0, 0, 10, 10, rgba(255, 0, 0, 255), ui_texture_id(white));
    // A second command with an empty clip rectangle is skipped.
    UiDrawCommand hidden = quad.commands[0];
    hidden.clip_max_x = 0.0f;
    quad.commands.push_back(hidden);
    // So is one wholly outside the 64x64 target (D3D12 clamps the same way).
    UiDrawCommand outside = quad.commands[0];
    outside.clip_min_x = 100.0f;
    outside.clip_max_x = 200.0f;
    quad.commands.push_back(outside);
    quad.lists = {{quad.vertices, quad.indices, quad.commands}};

    TextureDesc target_desc;
    target_desc.width = 64;
    target_desc.height = 64;
    target_desc.format = Format::rgba8_srgb;
    target_desc.usage = TextureUsage::render_target;
    const TextureHandle target = device->create_texture(target_desc);
    TextureDesc depth_desc = target_desc;
    depth_desc.format = Format::depth32_float;
    depth_desc.usage = TextureUsage::depth_target;
    const TextureHandle depth = device->create_texture(depth_desc);

    GraphicsPipelineDesc draw_desc;
    draw_desc.vertex = shaders::test_draw_vs();
    draw_desc.pixel = shaders::test_draw_ps();
    draw_desc.color_formats[0] = Format::rgba8_srgb;
    draw_desc.depth_format = Format::depth32_float;
    draw_desc.depth = DepthMode::test_write;
    const PipelineHandle draw = device->create_pipeline(draw_desc);
    ComputePipelineDesc fill_desc;
    fill_desc.compute = shaders::test_fill_cs();
    const PipelineHandle fill = device->create_pipeline(fill_desc);
    BufferDesc index_desc;
    index_desc.size = 12;
    index_desc.usage = BufferUsage::index;
    const std::array<u32, 3> tri{0, 1, 2};
    const BufferHandle indices =
        device->create_buffer(index_desc, {reinterpret_cast<const u8*>(tri.data()), 12});

    CommandList* cmd = device->begin_frame(nullptr);
    cmd->set_pipeline(fill);
    cmd->push(std::array<u32, 4>{0, 0, 0, 0});
    cmd->dispatch(2);
    cmd->barrier(indices, ResourceState::index_buffer);
    RenderPassDesc pass;
    pass.colors[0] = {target, LoadOp::clear, {}};
    pass.depth = {depth, LoadOp::clear, 0.0f};
    cmd->begin_render_pass(pass);
    cmd->set_pipeline(draw);
    cmd->draw(3);
    cmd->set_index_buffer(indices);
    cmd->draw_indexed(3, 2);
    cmd->end_render_pass();
    cmd->draw_ui(target, quad.frame());
    device->end_frame();

    const FrameStats& stats = device->last_frame_stats();
    GIANT_CHECK(stats.dispatches == 1);
    GIANT_CHECK(stats.ui_draws == 1);
    GIANT_CHECK(stats.draws == 3); // two scene draws and one UI draw

    const auto image = render_ui_offscreen(*device, quad.frame(), 8, 4, {1.0f, 0.5f, 0.0f, 1.0f});
    GIANT_CHECK(image.size() == 8 * 4 * 4);
    if (image.size() == 8 * 4 * 4) {
        GIANT_CHECK(near(pixel(image, 8, 7, 3), {255, 128, 0, 255}, 1));
    }
    for (const TextureHandle t : {white, target, depth}) {
        device->destroy(t);
    }
    device->destroy(draw);
    device->destroy(fill);
    device->destroy(indices);
}

GIANT_TEST("rhi: empty draws and dispatches are no-ops, as in D3D12") {
    auto device = null_device();
    if (device == nullptr) {
        GIANT_CHECK(false);
        return;
    }
    ComputePipelineDesc desc;
    const PipelineHandle pipeline = device->create_pipeline(desc);
    CountAsserts asserts;
    CommandList* cmd = device->begin_frame(nullptr);
    cmd->set_pipeline(pipeline);
    cmd->dispatch(0); // e.g. GPU culling of an empty scene
    device->end_frame();
    GIANT_CHECK(asserts.count() == 0);
    device->destroy(pipeline);
}

GIANT_TEST("rhi: the null device checks mesh shader and indirect calls") {
    auto device = null_device();
    if (device == nullptr) {
        GIANT_CHECK(false);
        return;
    }
    GIANT_CHECK(device->adapter().meets_minimum_spec()); // it stands in for one
    TextureDesc target_desc;
    target_desc.width = 8;
    target_desc.height = 8;
    target_desc.format = Format::rgba8_srgb;
    target_desc.usage = TextureUsage::render_target;
    const TextureHandle target = device->create_texture(target_desc);
    GraphicsPipelineDesc mesh_desc;
    mesh_desc.geometry = GeometryStage::mesh;
    mesh_desc.mesh = shaders::test_mesh_ms();
    mesh_desc.pixel = shaders::test_mesh_ps();
    mesh_desc.color_formats[0] = Format::rgba8_srgb;
    const PipelineHandle mesh = device->create_pipeline(mesh_desc);
    GraphicsPipelineDesc vertex_desc;
    vertex_desc.vertex = shaders::test_draw_vs();
    vertex_desc.pixel = shaders::test_draw_ps();
    vertex_desc.color_formats[0] = Format::rgba8_srgb;
    const PipelineHandle vertex = device->create_pipeline(vertex_desc);
    ComputePipelineDesc fill_desc;
    fill_desc.compute = shaders::test_fill_cs();
    const PipelineHandle fill = device->create_pipeline(fill_desc);
    BufferDesc args_desc;
    args_desc.size = 32;
    args_desc.usage = BufferUsage::indirect | BufferUsage::shader_write;
    const BufferHandle args = device->create_buffer(args_desc);
    BufferDesc plain_desc;
    plain_desc.size = 32;
    const BufferHandle plain = device->create_buffer(plain_desc);

    {
        // Used right, nothing asserts, and each call is counted.
        CountAsserts asserts;
        CommandList* cmd = device->begin_frame(nullptr);
        cmd->barrier(args, ResourceState::indirect_argument);
        cmd->set_pipeline(fill);
        cmd->dispatch_indirect(args, 4);
        RenderPassDesc pass;
        pass.colors[0] = {target, LoadOp::clear, {}};
        cmd->begin_render_pass(pass);
        cmd->set_pipeline(mesh);
        cmd->dispatch_mesh(4);
        cmd->dispatch_mesh_indirect(args, 0);
        cmd->set_pipeline(vertex);
        cmd->draw_indirect(args, 16);
        cmd->end_render_pass();
        device->end_frame();
        GIANT_CHECK(asserts.count() == 0);
        GIANT_CHECK(device->last_frame_stats().draws == 3);
        GIANT_CHECK(device->last_frame_stats().dispatches == 1);
    }
    {
        CountAsserts asserts;
        CommandList* cmd = device->begin_frame(nullptr);
        RenderPassDesc pass;
        pass.colors[0] = {target, LoadOp::clear, {}};
        cmd->begin_render_pass(pass);
        cmd->set_pipeline(mesh);
        cmd->draw(3); // a mesh pipeline is not drawn with draw()
        GIANT_CHECK(asserts.count() == 1);
        cmd->dispatch_mesh_indirect(args, 0); // not barriered to indirect_argument
        GIANT_CHECK(asserts.count() == 2);
        cmd->set_pipeline(vertex);
        cmd->dispatch_mesh(1); // a vertex pipeline is not a mesh pipeline
        GIANT_CHECK(asserts.count() == 3);
        cmd->end_render_pass();
        cmd->barrier(args, ResourceState::indirect_argument);
        cmd->barrier(plain, ResourceState::shader_read);
        cmd->set_pipeline(fill);
        cmd->dispatch_indirect(plain, 0); // no BufferUsage::indirect, wrong state
        GIANT_CHECK(asserts.count() == 5);
        cmd->dispatch_indirect(args, 24); // 12 bytes from 24 run past 32
        GIANT_CHECK(asserts.count() == 6);
        cmd->dispatch_indirect(args, 2); // unaligned
        GIANT_CHECK(asserts.count() == 7);
        cmd->begin_render_pass(pass);
        cmd->set_pipeline(mesh);
        cmd->dispatch_mesh(65536); // past the per-dimension limit
        GIANT_CHECK(asserts.count() == 8);
        cmd->end_render_pass();
        device->end_frame();
    }
    device->destroy(target);
    device->destroy(mesh);
    device->destroy(vertex);
    device->destroy(fill);
    device->destroy(args);
    device->destroy(plain);
}

GIANT_TEST("rhi: UI can show a texture rendered earlier in the frame") {
    auto device = null_device();
    if (device == nullptr) {
        GIANT_CHECK(false);
        return;
    }
    TextureDesc thumb_desc;
    thumb_desc.width = 8;
    thumb_desc.height = 8;
    thumb_desc.format = Format::rgba8_unorm;
    thumb_desc.usage = TextureUsage::render_target | TextureUsage::sampled;
    const TextureHandle thumbnail = device->create_texture(thumb_desc);
    TextureDesc target_desc = thumb_desc;
    target_desc.usage = TextureUsage::render_target;
    const TextureHandle target = device->create_texture(target_desc);
    UiQuad quad(0, 0, 8, 8, rgba(255, 255, 255, 255), ui_texture_id(thumbnail));
    CommandList* cmd = device->begin_frame(nullptr);
    RenderPassDesc pass;
    pass.colors[0] = {thumbnail, LoadOp::clear, {1, 0, 0, 1}};
    cmd->begin_render_pass(pass);
    cmd->end_render_pass();
    cmd->draw_ui(target, quad.frame()); // moves the thumbnail to shader_read itself
    device->end_frame();
    GIANT_CHECK(device->last_frame_stats().ui_draws == 1);
    GIANT_CHECK(device->last_frame_stats().barriers == 3); // thumbnail x2, target
    device->destroy(thumbnail);
    device->destroy(target);
}

GIANT_TEST("rhi: the null device catches misuse a GPU would crash on") {
    auto device = null_device();
    if (device == nullptr) {
        GIANT_CHECK(false);
        return;
    }
    TextureDesc target_desc;
    target_desc.width = 8;
    target_desc.height = 8;
    target_desc.format = Format::rgba8_srgb;
    target_desc.usage = TextureUsage::render_target;
    const TextureHandle target = device->create_texture(target_desc);
    TextureDesc other_desc = target_desc;
    other_desc.width = 4;
    const TextureHandle smaller = device->create_texture(other_desc);

    GraphicsPipelineDesc hdr_desc; // renders to rgba16_float, not the pass's format
    hdr_desc.color_formats[0] = Format::rgba16_float;
    const PipelineHandle hdr = device->create_pipeline(hdr_desc);

    BufferDesc index_desc;
    index_desc.size = 12;
    index_desc.usage = BufferUsage::index;
    const BufferHandle indices = device->create_buffer(index_desc);
    BufferDesc copy_desc;
    copy_desc.size = 16;
    const BufferHandle src = device->create_buffer(copy_desc);
    const BufferHandle dst = device->create_buffer(copy_desc);

    {
        CountAsserts asserts;
        CommandList* cmd = device->begin_frame(nullptr);
        RenderPassDesc pass;
        pass.colors[0] = {target, LoadOp::clear, {}};
        cmd->begin_render_pass(pass);
        cmd->set_pipeline(hdr);
        cmd->draw(3); // pipeline format does not match the pass
        GIANT_CHECK(asserts.count() == 1);
        cmd->set_index_buffer(indices); // never moved to index_buffer
        GIANT_CHECK(asserts.count() == 2);
        cmd->set_scissor(0, 0, 16, 16); // larger than the target
        GIANT_CHECK(asserts.count() == 3);
        cmd->end_render_pass();

        cmd->copy_buffer(dst, 0, src, 0, 16); // no copy_dest / copy_source barriers
        GIANT_CHECK(asserts.count() == 5);

        RenderPassDesc mixed;
        mixed.color_count = 2;
        mixed.colors[0] = {target, LoadOp::clear, {}};
        mixed.colors[1] = {smaller, LoadOp::clear, {}};
        cmd->begin_render_pass(mixed); // targets differ in size
        GIANT_CHECK(asserts.count() == 6);
        cmd->end_render_pass();
        cmd->dispatch(1); // no compute pipeline bound (the graphics one is)
        GIANT_CHECK(asserts.count() == 7);
        device->end_frame();
    }

    device->destroy(target);
    device->destroy(smaller);
    device->destroy(hdr);
    device->destroy(indices);
    device->destroy(src);
    device->destroy(dst);
}

GIANT_TEST("rhi: the null device takes passes without targets, sized by the pass") {
    auto device = null_device();
    if (device == nullptr) {
        GIANT_CHECK(false);
        return;
    }
    GraphicsPipelineDesc none_desc;
    none_desc.color_count = 0; // writes buffers only
    const PipelineHandle none = device->create_pipeline(none_desc);
    GIANT_CHECK(none.valid());
    TextureDesc target_desc;
    target_desc.width = 8;
    target_desc.height = 8;
    target_desc.format = Format::rgba8_srgb;
    target_desc.usage = TextureUsage::render_target;
    const TextureHandle target = device->create_texture(target_desc);
    {
        CountAsserts asserts;
        CommandList* cmd = device->begin_frame(nullptr);
        RenderPassDesc pass;
        pass.color_count = 0;
        pass.width = 2048;
        pass.height = 1024;
        cmd->begin_render_pass(pass);
        cmd->set_pipeline(none);
        cmd->set_scissor(1024, 512, 1024, 512); // inside the pass's own size
        cmd->draw(3);
        GIANT_CHECK(asserts.count() == 0);
        cmd->set_scissor(0, 0, 4096, 16); // outside it
        GIANT_CHECK(asserts.count() == 1);
        cmd->end_render_pass();
        RenderPassDesc unsized;
        unsized.color_count = 0;
        cmd->begin_render_pass(unsized); // no targets and no size
        GIANT_CHECK(asserts.count() == 2);
        cmd->end_render_pass();
        RenderPassDesc with_target;
        with_target.colors[0] = {target, LoadOp::clear, {}};
        cmd->begin_render_pass(with_target);
        cmd->set_pipeline(none);
        cmd->draw(3); // the pipeline has no targets, the pass has one
        GIANT_CHECK(asserts.count() == 3);
        cmd->end_render_pass();
        device->end_frame();
    }
    GIANT_CHECK(device->last_frame_stats().draws == 2);
    device->destroy(none);
    device->destroy(target);
}

GIANT_TEST("rhi: resources made inside a frame hold their data and count as uploads") {
    auto device = null_device();
    if (device == nullptr) {
        GIANT_CHECK(false);
        return;
    }
    BufferDesc readback_desc;
    readback_desc.size = 16;
    readback_desc.usage = BufferUsage::none;
    readback_desc.memory = Memory::readback;
    const BufferHandle readback = device->create_buffer(readback_desc);
    const std::array<u32, 4> values{5, 6, 7, 8};
    const std::span<const u8> bytes{reinterpret_cast<const u8*>(values.data()), 16};

    CommandList* cmd = device->begin_frame(nullptr);
    BufferDesc desc;
    desc.size = 16;
    const BufferHandle streamed = device->create_buffer(desc, bytes); // used this frame
    TextureDesc tex_desc;
    tex_desc.width = 2;
    tex_desc.height = 2;
    tex_desc.format = Format::rgba8_unorm;
    const TextureHandle texture = device->create_texture(tex_desc, bytes);
    cmd->barrier(streamed, ResourceState::copy_source);
    cmd->copy_buffer(readback, 0, streamed, 0, 16);
    device->end_frame();
    GIANT_CHECK(device->last_frame_stats().upload_bytes == 32);
    GIANT_CHECK(as_u32(device->read_buffer(readback)) == (std::vector<u32>{5, 6, 7, 8}));
    GIANT_CHECK(as_u32(device->read_texture(texture)) == (std::vector<u32>{5, 6, 7, 8}));

    // Outside a frame (load time) it is no frame's upload.
    const BufferHandle loaded = device->create_buffer(desc, bytes);
    device->begin_frame(nullptr);
    device->end_frame();
    GIANT_CHECK(device->last_frame_stats().upload_bytes == 0);

    for (const BufferHandle b : {readback, streamed, loaded}) {
        device->destroy(b);
    }
    device->destroy(texture);
}

GIANT_TEST("rhi: timing scopes are reported in the order begun, nested and capped") {
    auto device = null_device();
    if (device == nullptr) {
        GIANT_CHECK(false);
        return;
    }
    GIANT_CHECK(device->gpu_timings().empty()); // nothing timed yet
    CommandList* cmd = device->begin_frame(nullptr);
    cmd->begin_timing("culling");
    cmd->begin_timing("raster"); // nested
    cmd->end_timing();
    cmd->end_timing();
    const std::string shading = "shading"; // the name is copied
    cmd->begin_timing(shading.c_str());
    cmd->end_timing();
    device->end_frame();
    const auto timings = device->gpu_timings();
    GIANT_CHECK(timings.size() == 3);
    if (timings.size() == 3) {
        GIANT_CHECK(timings[0].name == "culling" && timings[1].name == "raster" &&
                    timings[2].name == "shading");
        GIANT_CHECK(timings[0].ms == 0.0f && timings[2].ms == 0.0f); // no GPU
    }
    GIANT_CHECK(device->gpu_frame_ms() == 0.0f);

    // Past the cap, scopes are not timed but still pair up.
    cmd = device->begin_frame(nullptr);
    for (u32 i = 0; i < k_max_gpu_timings + 8; ++i) {
        cmd->begin_timing("many");
        cmd->end_timing();
    }
    cmd->begin_timing("outer");
    cmd->end_timing();
    device->end_frame();
    GIANT_CHECK(device->gpu_timings().size() == k_max_gpu_timings);

    // An untimed frame reports nothing; misuse is caught.
    device->begin_frame(nullptr);
    device->end_frame();
    GIANT_CHECK(device->gpu_timings().empty());
    {
        CountAsserts asserts;
        cmd = device->begin_frame(nullptr);
        cmd->end_timing(); // nothing open
        GIANT_CHECK(asserts.count() == 1);
        cmd->begin_timing("left open");
        device->end_frame();
        GIANT_CHECK(asserts.count() == 2);
    }
}

// --- D3D12 on WARP --------------------------------------------------------------
//
// Runs on every Windows CI build, which has no GPU: WARP is Microsoft's
// software rasterizer, so these prove real D3D12 submission, bindless
// shaders and readback work, with the validation layer finding nothing.
#if GIANT_RHI_HAS_D3D12
namespace {
std::unique_ptr<Device> warp_device() {
    DeviceDesc desc;
    desc.backend = Backend::d3d12;
    desc.software_adapter = true;
    desc.debug_layer = true;
    auto device = create_device(desc);
    GIANT_CHECK(device != nullptr);
#if defined(GIANT_REQUIRE_VALIDATION)
    GIANT_CHECK(device == nullptr || device->validation_enabled());
#endif
    return device;
}
} // namespace

GIANT_TEST("rhi: d3d12 clears and reads back on the software adapter") {
    auto device = warp_device();
    if (device == nullptr) {
        return;
    }
    GIANT_CHECK(device->adapter().software);
    GIANT_CHECK(!device->adapter().name.empty());
    GIANT_CHECK(device->adapter().shader_model_6_6);
    check_color_near(clear_and_read_back(*device, {0.25f, 0.5f, 0.75f, 1.0f}),
                     {0.25f, 0.5f, 0.75f, 1.0f});
    // A second run reuses the frame's allocators.
    check_color_near(clear_and_read_back(*device, {1.0f, 0.0f, 0.0f, 1.0f}),
                     {1.0f, 0.0f, 0.0f, 1.0f});
    GIANT_CHECK(!device->device_lost());
    GIANT_CHECK(device->validation_errors() == 0);
}

GIANT_TEST("rhi: d3d12 draws UI with clipping and gamma-space blending") {
    auto device = warp_device();
    if (device == nullptr) {
        return;
    }
    const TextureHandle white = white_ui_texture(*device);
    GIANT_CHECK(white.valid());

    // A red rectangle over the whole image, clipped to the left half.
    const UiQuad red(0, 0, 64, 32, rgba(255, 0, 0, 255), ui_texture_id(white), 32.0f);
    const auto image = render_ui_offscreen(*device, red.frame(), 64, 32, {0.0f, 0.0f, 1.0f, 1.0f});
    GIANT_CHECK(image.size() == 64 * 32 * 4);
    if (image.size() == 64 * 32 * 4) {
        GIANT_CHECK(near(pixel(image, 64, 10, 10), {255, 0, 0, 255}));
        GIANT_CHECK(near(pixel(image, 64, 50, 10), {0, 0, 255, 255})); // clipped away
    }
    GIANT_CHECK(device->last_frame_stats().ui_draws == 1);

    // Half-transparent white over black blends to the halfway byte value,
    // as UI colours are picked, not the sRGB-encoded linear halfway point.
    const UiQuad grey(0, 0, 16, 16, rgba(255, 255, 255, 128), ui_texture_id(white));
    const auto blended = render_ui_offscreen(*device, grey.frame(), 16, 16, {0.0f, 0.0f, 0.0f, 1.0f});
    GIANT_CHECK(blended.size() == 16 * 16 * 4);
    if (blended.size() == 16 * 16 * 4) {
        GIANT_CHECK(std::abs(pixel(blended, 16, 8, 8)[0] - 128) <= 2);
    }

    // A texture is sampled: a 2x1 texture, black then white.
    TextureDesc ramp_desc;
    ramp_desc.width = 2;
    ramp_desc.height = 1;
    ramp_desc.format = Format::rgba8_unorm;
    ramp_desc.usage = TextureUsage::sampled;
    const std::vector<u8> ramp = {0, 0, 0, 255, 255, 255, 255, 255};
    const TextureHandle ramp_texture = device->create_texture(ramp_desc, ramp);
    const UiQuad textured(0, 0, 32, 8, rgba(255, 255, 255, 255), ui_texture_id(ramp_texture));
    const auto sampled =
        render_ui_offscreen(*device, textured.frame(), 32, 8, {1.0f, 0.0f, 0.0f, 1.0f});
    GIANT_CHECK(sampled.size() == 32 * 8 * 4);
    if (sampled.size() == 32 * 8 * 4) {
        GIANT_CHECK(pixel(sampled, 32, 1, 4)[0] < 40);   // black side
        GIANT_CHECK(pixel(sampled, 32, 30, 4)[1] > 215); // white side
    }
    device->destroy(ramp_texture);
    device->destroy(white);
    GIANT_CHECK(!device->device_lost());
    GIANT_CHECK(device->validation_errors() == 0);
}

GIANT_TEST("rhi: d3d12 draws signed distance UI: sharp edge where the distance is a half, and an outline") {
    auto device = warp_device();
    if (device == nullptr) {
        return;
    }
    // Distance rising left to right: 0 at the first texel, 1 at the last.
    TextureDesc desc;
    desc.width = 8;
    desc.height = 1;
    desc.format = Format::rgba8_unorm;
    desc.usage = TextureUsage::sampled;
    std::vector<u8> ramp;
    for (u32 i = 0; i < 8; ++i) {
        for (const u32 c : {255u, 255u, 255u, i * 255u / 7u}) ramp.push_back(static_cast<u8>(c));
    }
    const TextureHandle field = device->create_texture(desc, ramp);
    UiQuad quad(0, 0, 64, 8, rgba(255, 0, 0, 255), ui_texture_id(field));
    quad.commands[0].shade = UiShade::sdf;
    quad.lists = {{quad.vertices, quad.indices, quad.commands}};
    auto image = render_ui_offscreen(*device, quad.frame(), 64, 8, {0.0f, 0.0f, 1.0f, 1.0f});
    GIANT_CHECK(image.size() == 64 * 8 * 4);
    if (image.size() == 64 * 8 * 4) {
        GIANT_CHECK(near(pixel(image, 64, 4, 4), {0, 0, 255, 255}, 4));  // outside
        GIANT_CHECK(near(pixel(image, 64, 60, 4), {255, 0, 0, 255}, 4)); // inside
        GIANT_CHECK(near(pixel(image, 64, 22, 4), {0, 0, 255, 255}, 4)); // just outside (0.33)
        // The edge is about a pixel wide (the field is 8 texels over 64 pixels).
        int mixed = 0;
        for (u32 x = 0; x < 64; ++x) {
            const auto p = pixel(image, 64, x, 4);
            if (p[0] > 20 && p[2] > 20) ++mixed;
        }
        GIANT_CHECK(mixed <= 3);
    }
    // With an outline a quarter of the distance out: green between 0.25 and 0.5.
    quad.commands[0].outline = 0.25f;
    quad.commands[0].outline_color = rgba(0, 255, 0, 255);
    quad.lists = {{quad.vertices, quad.indices, quad.commands}};
    image = render_ui_offscreen(*device, quad.frame(), 64, 8, {0.0f, 0.0f, 1.0f, 1.0f});
    GIANT_CHECK(image.size() == 64 * 8 * 4);
    if (image.size() == 64 * 8 * 4) {
        GIANT_CHECK(near(pixel(image, 64, 22, 4), {0, 255, 0, 255}, 4)); // in the outline now
        GIANT_CHECK(near(pixel(image, 64, 4, 4), {0, 0, 255, 255}, 4));
        GIANT_CHECK(near(pixel(image, 64, 60, 4), {255, 0, 0, 255}, 4));
    }
    device->destroy(field);
    GIANT_CHECK(!device->device_lost());
    GIANT_CHECK(device->validation_errors() == 0);
}

GIANT_TEST("rhi: d3d12 compute writes a buffer read back through a copy") {
    auto device = warp_device();
    if (device == nullptr) {
        return;
    }
    ComputePipelineDesc desc;
    desc.compute = shaders::test_fill_cs();
    const PipelineHandle fill = device->create_pipeline(desc);
    GIANT_CHECK(fill.valid());

    constexpr u32 k_count = 100;
    BufferDesc out_desc;
    out_desc.size = k_count * 4;
    out_desc.usage = BufferUsage::shader_read | BufferUsage::shader_write;
    const BufferHandle out = device->create_buffer(out_desc);
    BufferDesc readback_desc = out_desc;
    readback_desc.usage = BufferUsage::none;
    readback_desc.memory = Memory::readback;
    const BufferHandle readback = device->create_buffer(readback_desc);

    CommandList* cmd = device->begin_frame(nullptr);
    GIANT_CHECK(cmd != nullptr);
    if (cmd != nullptr) {
        cmd->barrier(out, ResourceState::shader_write);
        cmd->set_pipeline(fill);
        cmd->push(std::array<u32, 4>{device->write_descriptor(out), k_count, 1000, 0});
        cmd->dispatch((k_count + 63) / 64);
        cmd->barrier(out, ResourceState::copy_source);
        cmd->copy_buffer(readback, 0, out, 0, k_count * 4);
        device->end_frame();
    }
    const auto values = as_u32(device->read_buffer(readback));
    GIANT_CHECK(values.size() == k_count);
    if (values.size() == k_count) {
        GIANT_CHECK(values[0] == 1000 && values[1] == 1002 && values[99] == 1198);
    }
    // read_buffer on the GPU buffer itself works too (it copies internally).
    GIANT_CHECK(as_u32(device->read_buffer(out)) == values);

    device->destroy(fill);
    device->destroy(out);
    device->destroy(readback);
    GIANT_CHECK(!device->device_lost());
    GIANT_CHECK(device->validation_errors() == 0);
}

GIANT_TEST("rhi: d3d12 compute writes a texture that is then sampled-ready") {
    auto device = warp_device();
    if (device == nullptr) {
        return;
    }
    ComputePipelineDesc desc;
    desc.compute = shaders::test_ramp_cs();
    const PipelineHandle ramp = device->create_pipeline(desc);
    TextureDesc tex_desc;
    tex_desc.width = 16;
    tex_desc.height = 4;
    tex_desc.format = Format::rgba8_unorm;
    tex_desc.usage = TextureUsage::shader_write | TextureUsage::sampled;
    const TextureHandle texture = device->create_texture(tex_desc);
    GIANT_CHECK(ramp.valid() && texture.valid());

    CommandList* cmd = device->begin_frame(nullptr);
    if (cmd != nullptr) {
        cmd->barrier(texture, ResourceState::shader_write);
        cmd->set_pipeline(ramp);
        cmd->push(std::array<u32, 4>{device->write_descriptor(texture), 16, 4, 0});
        cmd->dispatch(2, 1);
        cmd->barrier(texture, ResourceState::shader_read);
        device->end_frame();
    }
    const auto pixels = device->read_texture(texture);
    GIANT_CHECK(pixels.size() == 16 * 4 * 4);
    if (pixels.size() == 16 * 4 * 4) {
        GIANT_CHECK(near(pixel(pixels, 16, 0, 0), {0, 0, 255, 255}, 1));
        GIANT_CHECK(near(pixel(pixels, 16, 15, 3), {255, 0, 255, 255}, 1));
    }
    device->destroy(ramp);
    device->destroy(texture);
    GIANT_CHECK(device->validation_errors() == 0);
}

GIANT_TEST("rhi: d3d12 draws pulled triangles with reverse-Z depth testing") {
    auto device = warp_device();
    if (device == nullptr) {
        return;
    }
    constexpr u32 k_size = 32;
    TextureDesc color_desc;
    color_desc.width = k_size;
    color_desc.height = k_size;
    color_desc.format = Format::rgba8_srgb;
    color_desc.usage = TextureUsage::render_target;
    const TextureHandle color = device->create_texture(color_desc);
    TextureDesc depth_desc = color_desc;
    depth_desc.format = Format::depth32_float;
    depth_desc.usage = TextureUsage::depth_target;
    const TextureHandle depth = device->create_texture(depth_desc);

    GraphicsPipelineDesc desc;
    desc.vertex = shaders::test_draw_vs();
    desc.pixel = shaders::test_draw_ps();
    desc.color_formats[0] = Format::rgba8_srgb;
    desc.depth_format = Format::depth32_float;
    desc.depth = DepthMode::test_write;
    desc.cull = CullMode::back;
    const PipelineHandle pipeline = device->create_pipeline(desc);
    GIANT_CHECK(pipeline.valid());

    // A full-screen quad as two counter-clockwise triangles, from an index
    // buffer, plus one clockwise (back-facing) triangle that must be culled.
    const std::array<float, 8> quad{-1, -1, 1, -1, 1, 1, -1, 1};
    const std::array<u32, 6> quad_indices{0, 1, 2, 0, 2, 3};
    const std::array<float, 6> back_facing{-1, -1, -1, 1, 1, 1};
    BufferDesc index_desc;
    index_desc.size = sizeof(quad_indices);
    index_desc.usage = BufferUsage::index;
    const BufferHandle indices = device->create_buffer(
        index_desc, {reinterpret_cast<const u8*>(quad_indices.data()), sizeof(quad_indices)});

    struct DrawPush {
        float color[4];
        u32 positions;
        u32 position_base;
        float depth;
        u32 pad;
    };

    CommandList* cmd = device->begin_frame(nullptr);
    if (cmd != nullptr) {
        const TransientMemory quad_mem = cmd->upload_array(std::span<const float>(quad));
        const TransientMemory back_mem = cmd->upload_array(std::span<const float>(back_facing));
        cmd->barrier(indices, ResourceState::index_buffer);
        RenderPassDesc pass;
        pass.colors[0] = {color, LoadOp::clear, {0.0f, 0.0f, 0.0f, 1.0f}};
        pass.depth = {depth, LoadOp::clear, 0.0f};
        cmd->begin_render_pass(pass);
        cmd->set_pipeline(pipeline);
        cmd->set_index_buffer(indices);
        // Near green quad first, then a farther red one: depth keeps green.
        cmd->push(DrawPush{{0, 1, 0, 1}, quad_mem.descriptor, quad_mem.offset, 0.75f, 0});
        cmd->draw_indexed(6);
        cmd->push(DrawPush{{1, 0, 0, 1}, quad_mem.descriptor, quad_mem.offset, 0.25f, 0});
        cmd->draw_indexed(6);
        // A nearer blue triangle, but facing away: culled.
        cmd->push(DrawPush{{0, 0, 1, 1}, back_mem.descriptor, back_mem.offset, 0.9f, 0});
        cmd->draw(3);
        // Only the right half gets a nearer white quad (scissor).
        cmd->set_scissor(k_size / 2, 0, k_size / 2, k_size);
        cmd->push(DrawPush{{1, 1, 1, 1}, quad_mem.descriptor, quad_mem.offset, 1.0f, 0});
        cmd->draw_indexed(6);
        cmd->end_render_pass();
        device->end_frame();
    }
    const auto pixels = device->read_texture(color);
    GIANT_CHECK(pixels.size() == k_size * k_size * 4);
    if (pixels.size() == k_size * k_size * 4) {
        GIANT_CHECK(near(pixel(pixels, k_size, 4, 20), {0, 255, 0, 255}));
        GIANT_CHECK(near(pixel(pixels, k_size, 4, 2), {0, 255, 0, 255}));
        GIANT_CHECK(near(pixel(pixels, k_size, 28, 10), {255, 255, 255, 255}));
    }
    const auto depths = device->read_texture(depth);
    GIANT_CHECK(depths.size() == k_size * k_size * 4);
    if (depths.size() == k_size * k_size * 4) {
        float d = 0.0f;
        std::memcpy(&d, depths.data() + (10 * k_size + 4) * 4, 4);
        GIANT_CHECK_NEAR(d, 0.75, 1e-6);
    }
    GIANT_CHECK(device->last_frame_stats().draws == 4);

    device->destroy(pipeline);
    device->destroy(indices);
    device->destroy(color);
    device->destroy(depth);
    GIANT_CHECK(!device->device_lost());
    GIANT_CHECK(device->validation_errors() == 0);
}

GIANT_TEST("rhi: d3d12 draws a pass without targets that writes a buffer") {
    auto device = warp_device();
    if (device == nullptr) {
        return;
    }
    GraphicsPipelineDesc desc;
    desc.vertex = shaders::test_draw_vs();
    desc.pixel = shaders::test_draw_write_ps();
    desc.color_count = 0;
    desc.cull = CullMode::none;
    const PipelineHandle pipeline = device->create_pipeline(desc);
    GIANT_CHECK(pipeline.valid());
    BufferDesc out_desc;
    out_desc.size = 8 * 4 * 4;
    out_desc.usage = BufferUsage::shader_read | BufferUsage::shader_write;
    const BufferHandle out = device->create_buffer(out_desc, std::vector<u8>(8 * 4 * 4, 0));
    struct DrawPush {
        float color[4];
        u32 positions;
        u32 position_base;
        float depth;
        u32 target;
    };
    const std::array<float, 6> quad{-1, -1, 3, -1, -1, 3};
    CommandList* cmd = device->begin_frame(nullptr);
    if (cmd != nullptr) {
        const TransientMemory quad_mem = cmd->upload_array(std::span<const float>(quad));
        cmd->barrier(out, ResourceState::shader_write);
        RenderPassDesc pass;
        pass.color_count = 0;
        pass.width = 8;
        pass.height = 4;
        cmd->begin_render_pass(pass);
        cmd->set_pipeline(pipeline);
        const u32 target = device->write_descriptor(out);
        cmd->push(DrawPush{{}, quad_mem.descriptor, quad_mem.offset, 0.75f, target});
        cmd->draw(3);
        cmd->push(DrawPush{{}, quad_mem.descriptor, quad_mem.offset, 0.25f, target});
        cmd->draw(3);
        cmd->set_scissor(0, 0, 4, 4); // the left half only
        cmd->push(DrawPush{{}, quad_mem.descriptor, quad_mem.offset, 0.9f, target});
        cmd->draw(3);
        cmd->end_render_pass();
        device->end_frame();
    }
    const std::vector<u8> bytes = device->read_buffer(out);
    GIANT_CHECK(bytes.size() == 8 * 4 * 4);
    if (bytes.size() == 8 * 4 * 4) {
        const auto at = [&](u32 x, u32 y) {
            float d = 0.0f;
            std::memcpy(&d, bytes.data() + (y * 8 + x) * 4, 4);
            return d;
        };
        GIANT_CHECK_NEAR(at(1, 2), 0.9, 1e-6);
        GIANT_CHECK_NEAR(at(3, 0), 0.9, 1e-6);
        GIANT_CHECK_NEAR(at(4, 1), 0.75, 1e-6);
        GIANT_CHECK_NEAR(at(7, 3), 0.75, 1e-6);
    }
    device->destroy(pipeline);
    device->destroy(out);
    GIANT_CHECK(!device->device_lost());
    GIANT_CHECK(device->validation_errors() == 0);
}

GIANT_TEST("rhi: d3d12 runs GPU-written indirect dispatches and draws") {
    auto device = warp_device();
    if (device == nullptr) {
        return;
    }
    ComputePipelineDesc fill_desc;
    fill_desc.compute = shaders::test_fill_cs();
    const PipelineHandle fill = device->create_pipeline(fill_desc);
    GraphicsPipelineDesc draw_desc;
    draw_desc.vertex = shaders::test_draw_vs();
    draw_desc.pixel = shaders::test_draw_ps();
    draw_desc.color_formats[0] = Format::rgba8_srgb;
    draw_desc.cull = CullMode::none;
    const PipelineHandle draw = device->create_pipeline(draw_desc);
    GIANT_CHECK(fill.valid() && draw.valid());

    // Arguments: one 64-thread group of the fill (of 128 values), then a
    // draw of 3 vertices, 1 instance.
    const std::array<u32, 8> arg_values{1, 1, 1, 0, 3, 1, 0, 0};
    BufferDesc args_desc;
    args_desc.size = sizeof(arg_values);
    args_desc.usage = BufferUsage::indirect;
    const BufferHandle args = device->create_buffer(
        args_desc, {reinterpret_cast<const u8*>(arg_values.data()), sizeof(arg_values)});
    BufferDesc out_desc;
    out_desc.size = 128 * 4;
    out_desc.usage = BufferUsage::shader_read | BufferUsage::shader_write;
    const BufferHandle out = device->create_buffer(out_desc);
    constexpr u32 k_size = 16;
    TextureDesc color_desc;
    color_desc.width = k_size;
    color_desc.height = k_size;
    color_desc.format = Format::rgba8_srgb;
    color_desc.usage = TextureUsage::render_target;
    const TextureHandle color = device->create_texture(color_desc);
    // A triangle over the left half: (-1,-1), (0,-1)... as clip positions.
    const std::array<float, 6> tri{-1, -1, 0, -1, -1, 3};

    struct DrawPush {
        float color[4];
        u32 positions;
        u32 position_base;
        float depth;
        u32 pad;
    };
    CommandList* cmd = device->begin_frame(nullptr);
    if (cmd != nullptr) {
        const TransientMemory tri_mem = cmd->upload_array(std::span<const float>(tri));
        cmd->barrier(args, ResourceState::indirect_argument);
        cmd->barrier(out, ResourceState::shader_write);
        cmd->set_pipeline(fill);
        cmd->push(std::array<u32, 4>{device->write_descriptor(out), 128, 7, 0});
        cmd->dispatch_indirect(args, 0);
        RenderPassDesc pass;
        pass.colors[0] = {color, LoadOp::clear, {0.0f, 0.0f, 0.0f, 1.0f}};
        cmd->begin_render_pass(pass);
        cmd->set_pipeline(draw);
        cmd->push(DrawPush{{0, 1, 0, 1}, tri_mem.descriptor, tri_mem.offset, 0.5f, 0});
        cmd->draw_indirect(args, 16);
        cmd->end_render_pass();
        device->end_frame();
    }
    const auto values = as_u32(device->read_buffer(out));
    GIANT_CHECK(values.size() == 128);
    if (values.size() == 128) {
        GIANT_CHECK(values[0] == 7 && values[63] == 7 + 126);
        GIANT_CHECK(values[64] == 0 && values[127] == 0); // one group only
    }
    const auto pixels = device->read_texture(color);
    GIANT_CHECK(pixels.size() == k_size * k_size * 4);
    if (pixels.size() == k_size * k_size * 4) {
        GIANT_CHECK(near(pixel(pixels, k_size, 2, 8), {0, 255, 0, 255}));
        GIANT_CHECK(near(pixel(pixels, k_size, 13, 8), {0, 0, 0, 255}));
    }
    for (const PipelineHandle p : {fill, draw}) {
        device->destroy(p);
    }
    device->destroy(args);
    device->destroy(out);
    device->destroy(color);
    GIANT_CHECK(!device->device_lost());
    GIANT_CHECK(device->validation_errors() == 0);
}

GIANT_TEST("rhi: d3d12 mesh shaders draw directly and indirectly") {
    auto device = warp_device();
    if (device == nullptr) {
        return;
    }
    if (!device->adapter().mesh_shaders) {
        std::fprintf(stderr, "    skipped: %s has no mesh shaders\n", device->adapter().name.c_str());
        return;
    }
    GraphicsPipelineDesc desc;
    desc.geometry = GeometryStage::mesh;
    desc.mesh = shaders::test_mesh_ms();
    desc.pixel = shaders::test_mesh_ps();
    desc.color_formats[0] = Format::rgba8_srgb;
    desc.depth_format = Format::depth32_float;
    desc.depth = DepthMode::test_write;
    desc.cull = CullMode::back;
    const PipelineHandle pipeline = device->create_pipeline(desc);
    GIANT_CHECK(pipeline.valid());
    constexpr u32 k_size = 16;
    TextureDesc color_desc;
    color_desc.width = k_size;
    color_desc.height = k_size;
    color_desc.format = Format::rgba8_srgb;
    color_desc.usage = TextureUsage::render_target;
    const TextureHandle color = device->create_texture(color_desc);
    TextureDesc depth_desc = color_desc;
    depth_desc.format = Format::depth32_float;
    depth_desc.usage = TextureUsage::depth_target;
    const TextureHandle depth = device->create_texture(depth_desc);
    const std::array<u32, 3> arg_values{2, 1, 1}; // the top two of four strips
    BufferDesc args_desc;
    args_desc.size = 16;
    args_desc.usage = BufferUsage::indirect;
    const BufferHandle args = device->create_buffer(
        args_desc, {reinterpret_cast<const u8*>(arg_values.data()), sizeof(arg_values)});

    struct MeshPush {
        float color[4];
        u32 strips;
        float depth;
        u32 pad0;
        u32 pad1;
    };
    CommandList* cmd = device->begin_frame(nullptr);
    if (cmd != nullptr && pipeline.valid()) {
        cmd->barrier(args, ResourceState::indirect_argument);
        RenderPassDesc pass;
        pass.colors[0] = {color, LoadOp::clear, {0.0f, 0.0f, 0.0f, 1.0f}};
        pass.depth = {depth, LoadOp::clear, 0.0f};
        cmd->begin_render_pass(pass);
        cmd->set_pipeline(pipeline);
        // All four strips green, then the top two red and nearer.
        cmd->push(MeshPush{{0, 1, 0, 1}, 4, 0.25f, 0, 0});
        cmd->dispatch_mesh(4);
        cmd->push(MeshPush{{1, 0, 0, 1}, 4, 0.75f, 0, 0});
        cmd->dispatch_mesh_indirect(args, 0);
        cmd->end_render_pass();
        device->end_frame();
    }
    const auto pixels = device->read_texture(color);
    GIANT_CHECK(pixels.size() == k_size * k_size * 4);
    if (pixels.size() == k_size * k_size * 4) {
        GIANT_CHECK(near(pixel(pixels, k_size, 8, 2), {255, 0, 0, 255}));
        GIANT_CHECK(near(pixel(pixels, k_size, 8, 13), {0, 255, 0, 255}));
    }
    GIANT_CHECK(device->last_frame_stats().draws == 2);
    device->destroy(pipeline);
    device->destroy(args);
    device->destroy(color);
    device->destroy(depth);
    GIANT_CHECK(!device->device_lost());
    GIANT_CHECK(device->validation_errors() == 0);
}

GIANT_TEST("rhi: d3d12 frames keep going while resources are destroyed mid-flight") {
    auto device = warp_device();
    if (device == nullptr) {
        return;
    }
    // Many frames, each creating and destroying a buffer and allocating
    // transient memory: deferred destruction and per-frame memory recycle.
    for (u32 frame = 0; frame < 8; ++frame) {
        BufferDesc desc;
        desc.size = 256;
        const BufferHandle buffer = device->create_buffer(desc);
        CommandList* cmd = device->begin_frame(nullptr);
        GIANT_CHECK(cmd != nullptr);
        if (cmd == nullptr) {
            break;
        }
        const TransientMemory memory = cmd->allocate(64 * 1024);
        GIANT_CHECK(memory.data != nullptr);
        device->destroy(buffer); // still referenced by nothing, but mid-frame
        device->end_frame();
    }
    GIANT_CHECK(device->frames_completed() == 8);
    device->wait_idle();
    GIANT_CHECK(device->validation_errors() == 0);
}

GIANT_TEST("rhi: d3d12 places small buffers in shared heaps, reuses their room, and counts what it makes") {
    auto device = warp_device();
    if (device == nullptr) {
        return;
    }
    const DeviceCounts start = device->counts();
    GIANT_CHECK(start.heaps >= 1); // made at init
    const auto make = [&](u32 words, u32 seed) {
        std::vector<u32> values(words);
        for (u32 i = 0; i < words; ++i) values[i] = seed + i;
        BufferDesc desc;
        desc.size = words * 4;
        desc.usage = BufferUsage::shader_read | BufferUsage::shader_write;
        return std::pair{device->create_buffer(desc, {reinterpret_cast<const u8*>(values.data()), desc.size}), values};
    };
    // Made inside frames (staged, copied by the upload list), destroyed,
    // and made again in the room they left once the GPU is done.
    for (u32 round = 0; round < 3; ++round) {
        CommandList* cmd = device->begin_frame(nullptr);
        GIANT_CHECK(cmd != nullptr);
        if (cmd == nullptr) return;
        auto [small, values] = make(1024, round * 10'000);           // 4 KiB: placed
        auto [large, large_values] = make(1'200'000, round * 7);      // 4.6 MiB: committed
        device->end_frame();
        device->wait_idle();
        GIANT_CHECK(as_u32(device->read_buffer(small)) == values);
        GIANT_CHECK(as_u32(device->read_buffer(large)) == large_values);
        device->destroy(small);
        device->destroy(large);
        for (u32 i = 0; i <= Device::k_frames_in_flight; ++i) {
            if (device->begin_frame(nullptr) != nullptr) device->end_frame();
        }
    }
    device->wait_idle();
    const DeviceCounts end = device->counts();
    GIANT_CHECK(end.created - start.created >= 6);
    GIANT_CHECK(end.released - start.released >= 6);
    // The small ones took no memory of their own (the heaps had room); the
    // large ones did, one each.
    GIANT_CHECK(end.heaps == start.heaps);
    GIANT_CHECK(end.allocations - start.allocations >= 3);
    GIANT_CHECK(end.allocations - start.allocations < 6);
    GIANT_CHECK(!device->device_lost());
    GIANT_CHECK(device->validation_errors() == 0);
}

GIANT_TEST("rhi: d3d12 keeps a resource used this frame alive past a mid-frame upload") {
    auto device = warp_device();
    if (device == nullptr) {
        return;
    }
    GIANT_CHECK(device->adapter().agility_sdk_runtime);
    ComputePipelineDesc desc;
    desc.compute = shaders::test_fill_cs();
    const PipelineHandle fill = device->create_pipeline(desc);
    BufferDesc out_desc;
    out_desc.size = 256;
    out_desc.usage = BufferUsage::shader_read | BufferUsage::shader_write;
    // No read_buffer between frames: a readback waits for the GPU, which
    // would hide a resource freed too early. All are checked at the end.
    std::vector<BufferHandle> kept;
    for (u32 frame = 0; frame < 6; ++frame) {
        const BufferHandle used = device->create_buffer(out_desc);
        CommandList* cmd = device->begin_frame(nullptr);
        if (cmd == nullptr) {
            break;
        }
        cmd->barrier(used, ResourceState::shader_write);
        cmd->set_pipeline(fill);
        cmd->push(std::array<u32, 4>{device->write_descriptor(used), 64, frame, 0});
        cmd->dispatch(1);
        // Streaming: destroy what this frame uses, then upload something new.
        // The upload must not free `used` (or reuse its descriptor) before
        // this frame finishes.
        const u32 used_slots[2] = {device->read_descriptor(used), device->write_descriptor(used)};
        device->destroy(used);
        const std::vector<u8> data(256, 7);
        const BufferHandle streamed = device->create_buffer(out_desc, data);
        GIANT_CHECK(streamed.valid());
        // Descriptor slots are reused newest first, so a slot freed by the
        // upload would go to the next buffer created. It must not: this
        // frame's dispatch above still writes through it.
        const BufferHandle probe = device->create_buffer(out_desc);
        for (const BufferHandle b : {streamed, probe}) {
            for (const u32 slot : {device->read_descriptor(b), device->write_descriptor(b)}) {
                GIANT_CHECK(slot != used_slots[0] && slot != used_slots[1]);
            }
        }
        device->destroy(probe);
        cmd->barrier(streamed, ResourceState::shader_write);
        cmd->push(std::array<u32, 4>{device->write_descriptor(streamed), 64, 100 + frame, 0});
        cmd->dispatch(1);
        device->end_frame();
        kept.push_back(streamed);
    }
    for (u32 frame = 0; frame < kept.size(); ++frame) {
        // Each buffer holds its own frame's values: no write to a freed
        // buffer landed in a later one that reused its memory or descriptor.
        const std::vector<u8> bytes = device->read_buffer(kept[frame]);
        GIANT_CHECK(as_u32(bytes)[1] == 100 + frame + 2);
        GIANT_CHECK(as_u32(bytes)[63] == 100 + frame + 126);
        device->destroy(kept[frame]);
    }
    device->destroy(fill);
    device->wait_idle();
    GIANT_CHECK(!device->device_lost());
    GIANT_CHECK(device->validation_errors() == 0);
}

GIANT_TEST("rhi: d3d12 UI samples a texture rendered earlier in the frame") {
    auto device = warp_device();
    if (device == nullptr) {
        return;
    }
    TextureDesc thumb_desc;
    thumb_desc.width = 8;
    thumb_desc.height = 8;
    thumb_desc.format = Format::rgba8_unorm;
    thumb_desc.usage = TextureUsage::render_target | TextureUsage::sampled;
    const TextureHandle thumbnail = device->create_texture(thumb_desc);
    TextureDesc target_desc = thumb_desc;
    target_desc.width = 16;
    target_desc.usage = TextureUsage::render_target;
    const TextureHandle target = device->create_texture(target_desc);
    const UiQuad quad(0, 0, 16, 8, rgba(255, 255, 255, 255), ui_texture_id(thumbnail));
    for (int i = 0; i < 2; ++i) { // twice: the second frame starts in shader_read
        CommandList* cmd = device->begin_frame(nullptr);
        if (cmd == nullptr) {
            break;
        }
        RenderPassDesc pass;
        pass.colors[0] = {thumbnail, LoadOp::clear, {0, 1, 0, 1}};
        cmd->begin_render_pass(pass);
        cmd->end_render_pass();
        cmd->draw_ui(target, quad.frame());
        device->end_frame();
    }
    const auto pixels = device->read_texture(target);
    GIANT_CHECK(pixels.size() == 16 * 8 * 4);
    if (pixels.size() == 16 * 8 * 4) {
        GIANT_CHECK(near(pixel(pixels, 16, 8, 4), {0, 255, 0, 255}));
    }
    device->destroy(thumbnail);
    device->destroy(target);
    GIANT_CHECK(device->validation_errors() == 0);
}

GIANT_TEST("rhi: d3d12 uploads initial data inside a frame, used in that frame, without waiting") {
    auto device = warp_device();
    if (device == nullptr) {
        return;
    }
    constexpr u32 k_count = 256;
    constexpr u32 k_frames = 5; // more than the slots: each slot's staging is reused
    BufferDesc readback_desc;
    readback_desc.size = k_count * 4;
    readback_desc.usage = BufferUsage::none;
    readback_desc.memory = Memory::readback;
    std::vector<BufferHandle> readbacks;
    for (u32 i = 0; i < k_frames; ++i) {
        readbacks.push_back(device->create_buffer(readback_desc));
    }
    TextureDesc target_desc;
    target_desc.width = 16;
    target_desc.height = 8;
    target_desc.format = Format::rgba8_unorm;
    target_desc.usage = TextureUsage::render_target;
    const TextureHandle target = device->create_texture(target_desc);
    BufferHandle big; // over a staging chunk: split across chunks, one copy each
    constexpr u64 k_big_bytes = 9ull << 20;
    TextureHandle ramp;

    for (u32 frame = 0; frame < k_frames; ++frame) {
        CommandList* cmd = device->begin_frame(nullptr);
        GIANT_CHECK(cmd != nullptr);
        if (cmd == nullptr) {
            break;
        }
        std::vector<u32> values(k_count);
        for (u32 i = 0; i < k_count; ++i) {
            values[i] = frame * 1000 + i;
        }
        BufferDesc desc;
        desc.size = k_count * 4;
        desc.usage = BufferUsage::shader_read;
        desc.name = "streamed in a frame";
        const BufferHandle streamed = device->create_buffer(
            desc, {reinterpret_cast<const u8*>(values.data()), values.size() * 4});
        GIANT_CHECK(streamed.valid());
        cmd->barrier(streamed, ResourceState::copy_source);
        cmd->copy_buffer(readbacks[frame], 0, streamed, 0, k_count * 4);
        device->destroy(streamed); // destroyed in the frame that uploads and uses it
        u64 expected = u64{k_count} * 4;
        if (frame == 1) {
            // A texture made in the frame and sampled by its UI.
            TextureDesc ramp_desc;
            ramp_desc.width = 2;
            ramp_desc.height = 1;
            ramp_desc.format = Format::rgba8_unorm;
            ramp_desc.usage = TextureUsage::sampled;
            const std::vector<u8> pixels = {0, 0, 0, 255, 255, 255, 255, 255};
            ramp = device->create_texture(ramp_desc, pixels);
            GIANT_CHECK(ramp.valid());
            RenderPassDesc pass;
            pass.colors[0] = {target, LoadOp::clear, {1.0f, 0.0f, 0.0f, 1.0f}};
            cmd->begin_render_pass(pass);
            cmd->end_render_pass();
            const UiQuad quad(0, 0, 16, 8, rgba(255, 255, 255, 255), ui_texture_id(ramp));
            cmd->draw_ui(target, quad.frame());
            std::vector<u8> big_data(k_big_bytes);
            for (usize i = 0; i < big_data.size(); ++i) {
                big_data[i] = static_cast<u8>(i * 7);
            }
            BufferDesc big_desc;
            big_desc.size = k_big_bytes;
            big_desc.name = "large upload in a frame";
            big = device->create_buffer(big_desc, big_data);
            GIANT_CHECK(big.valid());
            expected += pixels.size() + k_big_bytes;
        }
        device->end_frame();
        GIANT_CHECK(device->last_frame_stats().upload_bytes == expected);
    }
    device->wait_idle();
    for (u32 frame = 0; frame < k_frames; ++frame) {
        const auto values = as_u32(device->read_buffer(readbacks[frame]));
        GIANT_CHECK(values.size() == k_count);
        if (values.size() == k_count) {
            GIANT_CHECK(values[0] == frame * 1000 && values[k_count - 1] == frame * 1000 + k_count - 1);
        }
        device->destroy(readbacks[frame]);
    }
    const auto image = device->read_texture(target);
    GIANT_CHECK(image.size() == 16 * 8 * 4);
    if (image.size() == 16 * 8 * 4) {
        GIANT_CHECK(pixel(image, 16, 1, 4)[0] < 40);   // the black texel
        GIANT_CHECK(pixel(image, 16, 14, 4)[1] > 215); // the white one
    }
    const auto big_back = device->read_buffer(big);
    GIANT_CHECK(big_back.size() == k_big_bytes);
    if (big_back.size() == k_big_bytes) {
        GIANT_CHECK(big_back[1] == 7 && big_back[k_big_bytes - 1] == static_cast<u8>((k_big_bytes - 1) * 7));
    }
    device->destroy(big);
    device->destroy(ramp);
    device->destroy(target);
    device->wait_idle();
    GIANT_CHECK(!device->device_lost());
    GIANT_CHECK(device->validation_errors() == 0);
}

GIANT_TEST("rhi: d3d12 times scopes with GPU timestamps, read without waiting") {
    auto device = warp_device();
    if (device == nullptr) {
        return;
    }
    ComputePipelineDesc desc;
    desc.compute = shaders::test_fill_cs();
    const PipelineHandle fill = device->create_pipeline(desc);
    constexpr u32 k_count = 1u << 16;
    BufferDesc out_desc;
    out_desc.size = k_count * 4;
    out_desc.usage = BufferUsage::shader_read | BufferUsage::shader_write;
    const BufferHandle out = device->create_buffer(out_desc);
    BufferDesc readback_desc = out_desc;
    readback_desc.usage = BufferUsage::none;
    readback_desc.memory = Memory::readback;
    const BufferHandle readback = device->create_buffer(readback_desc);
    for (u32 frame = 0; frame < 4; ++frame) {
        CommandList* cmd = device->begin_frame(nullptr);
        if (cmd == nullptr) {
            break;
        }
        // The last frame names its scopes apart: the newest finished frame wins.
        const bool last = frame == 3;
        cmd->begin_timing(last ? "fill (last)" : "fill");
        cmd->barrier(out, ResourceState::shader_write);
        cmd->set_pipeline(fill);
        cmd->push(std::array<u32, 4>{device->write_descriptor(out), k_count, frame, 0});
        cmd->dispatch(k_count / 64);
        cmd->end_timing();
        cmd->begin_timing("copy");
        cmd->barrier(out, ResourceState::copy_source);
        cmd->copy_buffer(readback, 0, out, 0, k_count * 4);
        cmd->end_timing();
        device->end_frame();
    }
    device->wait_idle(); // every frame finished: the newest one's times
    const auto timings = device->gpu_timings();
    GIANT_CHECK(timings.size() == 2);
    if (timings.size() == 2) {
        GIANT_CHECK(timings[0].name == "fill (last)" && timings[1].name == "copy");
        const float total = device->gpu_frame_ms();
        for (const GpuTiming& t : timings) {
            GIANT_CHECK(t.ms >= 0.0f);
            GIANT_CHECK(total + 1e-4f >= t.ms);
        }
        GIANT_CHECK(total + 1e-4f >= timings[0].ms + timings[1].ms); // one after the other
    }
    device->destroy(fill);
    device->destroy(out);
    device->destroy(readback);
    GIANT_CHECK(!device->device_lost());
    GIANT_CHECK(device->validation_errors() == 0);
}

GIANT_TEST("rhi: d3d12 refuses a swap chain without a window") {
    DeviceDesc desc;
    desc.backend = Backend::d3d12;
    desc.software_adapter = true;
    auto device = create_device(desc);
    GIANT_CHECK(device != nullptr);
    if (device != nullptr) {
        SwapChainDesc sc_desc;
        sc_desc.width = 64;
        sc_desc.height = 64;
        GIANT_CHECK(device->create_swap_chain(sc_desc) == nullptr);
    }
}
#endif

GIANT_TEST("rhi: small buffers' heap ranges fit best, join when freed, and never overlap") {
    using giant::rhi::HeapRanges;
    HeapRanges r(16);
    GIANT_CHECK(!r.allocate(1).valid()); // no heap yet
    GIANT_CHECK(r.add_heap() == 0);
    GIANT_CHECK(!r.allocate(0).valid() && !r.allocate(17).valid());
    const HeapRanges::Range a = r.allocate(4); // [0, 4)
    const HeapRanges::Range b = r.allocate(4); // [4, 8)
    const HeapRanges::Range c = r.allocate(8); // [8, 16)
    GIANT_CHECK(a.valid() && b.valid() && c.valid() && r.used() == 16);
    GIANT_CHECK(a.offset == 0 && b.offset == 4 && c.offset == 8);
    GIANT_CHECK(!r.allocate(1).valid()); // full
    r.free(b);
    r.free(a); // joins with b's room: [0, 8) free
    const HeapRanges::Range d = r.allocate(8);
    GIANT_CHECK(d.valid() && d.heap == 0 && d.offset == 0 && r.used() == 16);
    // A second heap; best fit takes the tightest room anywhere.
    GIANT_CHECK(r.add_heap() == 1 && r.heaps() == 2);
    r.free(c); // heap 0: [8, 16) free
    const HeapRanges::Range e = r.allocate(8);
    GIANT_CHECK(e.heap == 0 && e.offset == 8); // exact fit in heap 0, not heap 1's 16
    const HeapRanges::Range f = r.allocate(3);
    GIANT_CHECK(f.heap == 1 && f.offset == 0);
    r.free(d);
    r.free(e);
    r.free(f);
    GIANT_CHECK(r.used() == 0);
    // Everything joined again: a whole heap fits in each.
    const HeapRanges::Range g = r.allocate(16);
    const HeapRanges::Range h = r.allocate(16);
    GIANT_CHECK(g.valid() && h.valid() && g.heap != h.heap);
    // A range freed between two free ones joins all three.
    r.free(g);
    const HeapRanges::Range x = r.allocate(4), y = r.allocate(4), z = r.allocate(4);
    GIANT_CHECK(x.heap == y.heap && y.heap == z.heap && x.offset == 0 && y.offset == 4 && z.offset == 8);
    r.free(x);
    r.free(z);
    r.free(y);
    GIANT_CHECK(r.allocate(16).valid());
}

GIANT_TEST("rhi: texture sizes count blocks, mips and layers; unsound descriptions are refused") {
    GIANT_CHECK(is_block_compressed(Format::bc7_srgb) && !is_block_compressed(Format::rgba8_srgb));
    GIANT_CHECK(block_bytes(Format::bc1_unorm) == 8 && block_bytes(Format::bc4_unorm) == 8 &&
                block_bytes(Format::bc5_unorm) == 16 && block_bytes(Format::bc7_unorm) == 16);
    GIANT_CHECK(bytes_per_pixel(Format::bc1_srgb) == 0);
    // Blocks round up: a 2x2 level is still one block.
    GIANT_CHECK(level_bytes(Format::bc1_unorm, 8, 8) == 32 && level_bytes(Format::bc1_unorm, 2, 2) == 8);
    GIANT_CHECK(level_row_bytes(Format::bc7_unorm, 12) == 48 && level_rows(Format::bc7_unorm, 5) == 2);
    GIANT_CHECK(level_bytes(Format::rgba8_unorm, 3, 2) == 24);
    GIANT_CHECK(full_mip_count(1, 1) == 1 && full_mip_count(8, 8) == 4 && full_mip_count(1024, 4) == 11);
    TextureDesc d;
    d.width = 8;
    d.height = 8;
    d.format = Format::bc1_srgb;
    d.mips = 4; // 8, 4, 2, 1: 4 + 1 + 1 + 1 blocks
    d.layers = 3;
    d.array = true;
    GIANT_CHECK(texture_bytes(d) == 3u * (32 + 8 + 8 + 8));
    GIANT_CHECK(texture_desc_problem(d) == nullptr);
    TextureDesc bad = d;
    bad.mips = 5;
    GIANT_CHECK(texture_desc_problem(bad) != nullptr); // past the full chain
    bad = d;
    bad.width = 6;
    bad.mips = 1;
    GIANT_CHECK(texture_desc_problem(bad) != nullptr); // blocks need multiples of 4
    bad = d;
    bad.usage = TextureUsage::sampled | TextureUsage::shader_write;
    GIANT_CHECK(texture_desc_problem(bad) != nullptr); // compressed: sampled only
    bad = d;
    bad.format = Format::rgba8_unorm;
    bad.usage = TextureUsage::sampled | TextureUsage::render_target;
    GIANT_CHECK(texture_desc_problem(bad) != nullptr); // mips and layers: sampled only
    bad.mips = 1;
    bad.layers = 1;
    GIANT_CHECK(texture_desc_problem(bad) == nullptr);
    bad = d;
    bad.layers = 0;
    GIANT_CHECK(texture_desc_problem(bad) != nullptr);
    bad = d;
    bad.array = false;
    GIANT_CHECK(texture_desc_problem(bad) != nullptr); // layers need an array
    bad.layers = 1;
    GIANT_CHECK(texture_desc_problem(bad) == nullptr);
    bad.array = true; // a one-layer array is fine
    GIANT_CHECK(texture_desc_problem(bad) == nullptr);
    bad = d;
    bad.format = static_cast<Format>(250); // out of range, as from a corrupt file
    GIANT_CHECK(texture_desc_problem(bad) != nullptr);
    bad = d;
    bad.width = 16388;
    GIANT_CHECK(texture_desc_problem(bad) != nullptr);
    bad.width = 16384;
    bad.height = 16384;
    bad.mips = 1;
    bad.layers = 2048;
    GIANT_CHECK(texture_desc_problem(bad) != nullptr); // 2048 layers of 128 MiB: past 4 GiB
    bad.layers = 32;
    GIANT_CHECK(texture_desc_problem(bad) == nullptr); // 4 GiB exactly
    bad.layers = 2049;
    GIANT_CHECK(texture_desc_problem(bad) != nullptr);
    bad = d;
    bad.format = Format::depth32_float;
    bad.mips = 1;
    bad.layers = 1;
    bad.array = false;
    GIANT_CHECK(texture_desc_problem(bad) != nullptr); // depth formats need depth usage
    bad.usage = TextureUsage::depth_target | TextureUsage::shader_write;
    GIANT_CHECK(texture_desc_problem(bad) != nullptr);
    bad.usage = TextureUsage::depth_target | TextureUsage::sampled;
    GIANT_CHECK(texture_desc_problem(bad) == nullptr);
    bad.format = Format::rgba8_unorm;
    GIANT_CHECK(texture_desc_problem(bad) != nullptr); // depth usage needs a depth format
    bad = d;
    bad.width = 0;
    GIANT_CHECK(texture_desc_problem(bad) != nullptr);
    bad = d;
    bad.usage = TextureUsage::none;
    GIANT_CHECK(texture_desc_problem(bad) != nullptr);
    // Contents: empty or whole; compressed ones always whole.
    GIANT_CHECK(texture_contents_problem(d, texture_bytes(d)) == nullptr);
    GIANT_CHECK(texture_contents_problem(d, 0) != nullptr);
    GIANT_CHECK(texture_contents_problem(d, texture_bytes(d) - 8) != nullptr);
    // A 12x12 chain pads its small levels to whole blocks: 9, 4, 1, 1.
    TextureDesc odd;
    odd.width = 12;
    odd.height = 12;
    odd.format = Format::bc7_srgb;
    odd.mips = full_mip_count(12, 12);
    GIANT_CHECK(odd.mips == 4 && texture_bytes(odd) == (9 + 4 + 1 + 1) * 16u);
    // The null device refuses them too, and compressed ones without contents.
    DeviceDesc desc;
    desc.backend = Backend::null;
    auto device = create_device(desc);
    bad = d;
    bad.mips = 5;
    std::vector<u8> contents(texture_bytes(d), 0);
    {
        // Refused with a log, never an assert: bad loaded data must not abort.
        CountAsserts asserts;
        GIANT_CHECK(!device->create_texture(bad, contents).valid());
        GIANT_CHECK(!device->create_texture(d).valid());
        GIANT_CHECK(!device->create_texture(d, std::span<const u8>(contents).first(contents.size() - 8)).valid());
        bad = d;
        bad.width = 0;
        GIANT_CHECK(!device->create_texture(bad, contents).valid());
        bad = d;
        bad.format = Format::depth32_float;
        bad.mips = 1;
        bad.layers = 1;
        bad.array = false;
        GIANT_CHECK(!device->create_texture(bad).valid());
        GIANT_CHECK(asserts.count() == 0);
    }
    const TextureHandle t = device->create_texture(d, contents);
    GIANT_CHECK(t.valid() && device->read_descriptor(t) != k_no_descriptor);
    GIANT_CHECK(device->read_texture(t).empty()); // compressed: not read back, as on D3D12
    // An uncompressed texture with mips reads back its largest level.
    TextureDesc plain;
    plain.width = 4;
    plain.height = 2;
    plain.format = Format::rgba8_unorm;
    plain.mips = 3;
    std::vector<u8> levels(texture_bytes(plain));
    for (usize i = 0; i < levels.size(); ++i) {
        levels[i] = static_cast<u8>(i);
    }
    GIANT_CHECK(levels.size() == (8 + 2 + 1) * 4u);
    const TextureHandle p = device->create_texture(plain, levels);
    const auto back = device->read_texture(p);
    GIANT_CHECK(back.size() == 32 && back[0] == 0 && back[31] == 31);
}

#if GIANT_RHI_HAS_D3D12
namespace {
// A 4x4 block of BC1 in one colour (8-bit RGB rounded to 5:6:5).
std::array<u8, 8> bc1_solid(u8 r, u8 g, u8 b) {
    const u16 c = static_cast<u16>(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    return {static_cast<u8>(c & 0xFF), static_cast<u8>(c >> 8), static_cast<u8>(c & 0xFF), static_cast<u8>(c >> 8),
            0, 0, 0, 0};
}
// A 4x4 block of BC4 in one value.
std::array<u8, 8> bc4_solid(u8 v) { return {v, v, 0, 0, 0, 0, 0, 0}; }
} // namespace

GIANT_TEST("rhi: d3d12 samples block-compressed texture arrays at each mip and layer") {
    auto device = warp_device();
    if (device == nullptr) {
        return;
    }
    // Two layers of BC1, 8x8 with a 4x4 mip: layer 0 red then green, layer 1 blue then white.
    TextureDesc array;
    array.width = 8;
    array.height = 8;
    array.format = Format::bc1_unorm;
    array.mips = 2;
    array.layers = 2;
    array.array = true;
    std::vector<u8> contents;
    const auto add = [&](const std::array<u8, 8>& block, int count) {
        for (int i = 0; i < count; ++i) contents.insert(contents.end(), block.begin(), block.end());
    };
    add(bc1_solid(255, 0, 0), 4);
    add(bc1_solid(0, 255, 0), 1);
    add(bc1_solid(0, 0, 255), 4);
    add(bc1_solid(255, 255, 255), 1);
    GIANT_CHECK(contents.size() == texture_bytes(array));
    const TextureHandle colours = device->create_texture(array, contents);
    GIANT_CHECK(colours.valid());
    // One BC4 layer, 8x8 with mips down to 1x1: 0.25 at the top, 0.75 below.
    TextureDesc single;
    single.width = 8;
    single.height = 8;
    single.format = Format::bc4_unorm;
    single.mips = 4;
    std::vector<u8> values;
    for (int i = 0; i < 4; ++i) {
        const auto b = bc4_solid(64);
        values.insert(values.end(), b.begin(), b.end());
    }
    for (int i = 0; i < 3; ++i) {
        const auto b = bc4_solid(191);
        values.insert(values.end(), b.begin(), b.end());
    }
    GIANT_CHECK(values.size() == texture_bytes(single));
    const TextureHandle mask = device->create_texture(single, values);
    GIANT_CHECK(mask.valid());
    GIANT_CHECK(device->read_texture(mask).empty()); // compressed: not read back
    // A one-layer sRGB BC1 array, 12x12 so its small levels are block padded
    // (12, 6, 3: 9, 4 and 1 blocks): magenta at the top, cyan at mip 2. It
    // still reads as an array.
    TextureDesc lone;
    lone.width = 12;
    lone.height = 12;
    lone.format = Format::bc1_srgb;
    lone.mips = 3;
    lone.array = true;
    std::vector<u8> lone_blocks;
    const auto add_lone = [&](const std::array<u8, 8>& block, int count) {
        for (int i = 0; i < count; ++i) lone_blocks.insert(lone_blocks.end(), block.begin(), block.end());
    };
    add_lone(bc1_solid(255, 0, 255), 9);
    add_lone(bc1_solid(0, 0, 0), 4);
    add_lone(bc1_solid(0, 255, 255), 1);
    GIANT_CHECK(lone_blocks.size() == texture_bytes(lone));
    const TextureHandle one_layer = device->create_texture(lone, lone_blocks);
    GIANT_CHECK(one_layer.valid());

    ComputePipelineDesc pd;
    pd.compute = shaders::test_sample_cs();
    const PipelineHandle sample = device->create_pipeline(pd);
    GIANT_CHECK(sample.valid());
    BufferDesc out_desc;
    out_desc.size = 8 * 16;
    out_desc.usage = BufferUsage::shader_read | BufferUsage::shader_write;
    const BufferHandle out = device->create_buffer(out_desc);
    CommandList* cmd = device->begin_frame(nullptr);
    GIANT_CHECK(cmd != nullptr);
    if (cmd != nullptr) {
        cmd->barrier(colours, ResourceState::shader_read);
        cmd->barrier(mask, ResourceState::shader_read);
        cmd->barrier(one_layer, ResourceState::shader_read);
        cmd->barrier(out, ResourceState::shader_write);
        cmd->set_pipeline(sample);
        cmd->push(std::array<u32, 4>{device->write_descriptor(out), device->read_descriptor(colours),
                                     device->read_descriptor(mask), device->read_descriptor(one_layer)});
        cmd->dispatch(1);
        device->end_frame();
    }
    const std::vector<u8> bytes = device->read_buffer(out);
    GIANT_CHECK(bytes.size() == 8 * 16);
    if (bytes.size() == 8 * 16) {
        float f[32];
        std::memcpy(f, bytes.data(), sizeof(f));
        const auto near = [](float a, float b) { return std::abs(a - b) < 0.02f; };
        std::printf("bc sample: %.2f %.2f %.2f / %.2f %.2f %.2f / %.2f %.2f %.2f / %.2f %.2f %.2f / %.2f / %.2f\n",
                    f[0], f[1], f[2], f[4], f[5], f[6], f[8], f[9], f[10], f[12], f[13], f[14], f[16], f[20]);
        GIANT_CHECK(near(f[0], 1.0f) && near(f[1], 0.0f) && near(f[2], 0.0f));   // layer 0, mip 0: red
        GIANT_CHECK(near(f[4], 0.0f) && near(f[5], 1.0f) && near(f[6], 0.0f));   // layer 0, mip 1: green
        GIANT_CHECK(near(f[8], 0.0f) && near(f[9], 0.0f) && near(f[10], 1.0f));  // layer 1, mip 0: blue
        GIANT_CHECK(near(f[12], 1.0f) && near(f[13], 1.0f) && near(f[14], 1.0f)); // layer 1, mip 1: white
        GIANT_CHECK(near(f[16], 64.0f / 255.0f));                                 // BC4, mip 0
        GIANT_CHECK(near(f[20], 191.0f / 255.0f));                                // BC4, mip 1
        std::printf("one-layer array: %.2f %.2f %.2f / %.2f %.2f %.2f\n", f[24], f[25], f[26], f[28], f[29], f[30]);
        GIANT_CHECK(near(f[24], 1.0f) && near(f[25], 0.0f) && near(f[26], 1.0f)); // mip 0: magenta (sRGB view)
        GIANT_CHECK(near(f[28], 0.0f) && near(f[29], 1.0f) && near(f[30], 1.0f)); // mip 2: cyan
    }
    device->destroy(sample);
    device->destroy(out);
    device->destroy(colours);
    device->destroy(mask);
    device->destroy(one_layer);
    GIANT_CHECK(!device->device_lost());
    GIANT_CHECK(device->validation_errors() == 0);
}
#endif
